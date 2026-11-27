// Farview MV3 Chrome Native Messaging transport & cross-platform unpacked extension walker.
// Implements Chrome's standard 4-byte native-endian uint32 length-prefixed JSON stdio framing
// with non-blocking bounded outgoing queues, generation-tagged handles, 1 MiB incoming bounds,
// POSIX fd/signal hygiene, Windows Registry lookup + UTF-16 support, and clean teardown.

#include "native_messaging.h"
#include "../win_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <spawn.h>
extern char** environ;
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

typedef struct {
  int in_use;
  uint32_t gen;
#ifdef _WIN32
  int zombie;
  HANDLE h_proc;
  HANDLE h_stdin_w;
  HANDLE h_stdout_r;
  CRITICAL_SECTION tx_lock;
  HANDLE tx_event;
  HANDLE tx_thread;
  volatile LONG tx_stop;
  volatile LONG tx_failed;
  int tx_sync_init;
#else
  pid_t pid;
  int fd_stdin_w;
  int fd_stdout_r;
#endif
  // Non-blocking outgoing byte ring/queue
  uint8_t* tx_buf;
  size_t tx_len;
  size_t tx_off;
  size_t tx_cap;
  // Incoming 4-byte length-prefixed frame state
  uint8_t len_buf[4];
  int len_got;
  uint32_t msg_len;
  uint8_t* msg_buf;
  uint32_t msg_got;
  int msg_ready;
} FvNmSlot;

static FvNmSlot g_nm_slots[FV_NM_MAX_PORTS];
static uint32_t g_nm_gen_seq = 1;
static int g_nm_atexit_registered = 0;

static int32_t nm_make_handle(int idx, uint32_t gen) {
  return (int32_t)(((gen & 0x7FFFFFu) << 8) | (uint32_t)(idx + 1));
}

static FvNmSlot* nm_lookup(int32_t handle) {
  if (handle <= 0) return NULL;
  uint32_t h = (uint32_t)handle;
  int idx = (int)(h & 0xFFu) - 1;
  uint32_t gen = h >> 8;
  if (idx < 0 || idx >= FV_NM_MAX_PORTS || gen == 0) return NULL;
  FvNmSlot* slot = &g_nm_slots[idx];
  if (!slot->in_use || slot->gen != gen) return NULL;
  return slot;
}

static char* nm_dup_str(const char* s, int32_t len) {
  if (!s || len < 0) return NULL;
  char* p = (char*)malloc((size_t)len + 1);
  if (!p) return NULL;
  if (len > 0) memcpy(p, s, (size_t)len);
  p[len] = '\0';
  return p;
}

static void nm_reset_rx(FvNmSlot* slot) {
  slot->len_got = 0;
  slot->msg_len = 0;
  free(slot->msg_buf);
  slot->msg_buf = NULL;
  slot->msg_got = 0;
  slot->msg_ready = 0;
}

static void nm_reset_tx(FvNmSlot* slot) {
  free(slot->tx_buf);
  slot->tx_buf = NULL;
  slot->tx_len = 0;
  slot->tx_off = 0;
  slot->tx_cap = 0;
}

#ifdef _WIN32
static DWORD WINAPI nm_win_writer_proc(LPVOID arg) {
  FvNmSlot* slot = (FvNmSlot*)arg;
  for (;;) {
    WaitForSingleObject(slot->tx_event, 50);
    for (;;) {
      uint8_t chunk[16384];
      size_t take = 0;
      HANDLE hw = NULL;
      EnterCriticalSection(&slot->tx_lock);
      LONG stop_now = InterlockedCompareExchange(&slot->tx_stop, 0, 0);
      if (!stop_now && slot->tx_off < slot->tx_len) {
        size_t avail = slot->tx_len - slot->tx_off;
        take = avail < sizeof(chunk) ? avail : sizeof(chunk);
        memcpy(chunk, slot->tx_buf + slot->tx_off, take);
        slot->tx_off += take;
        if (slot->tx_off == slot->tx_len) {
          slot->tx_off = 0;
          slot->tx_len = 0;
        }
        hw = slot->h_stdin_w;
      }
      LeaveCriticalSection(&slot->tx_lock);

      if (stop_now) return 0;
      if (take == 0) break;
      if (!hw || hw == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&slot->tx_failed, 1);
        return 1;
      }
      size_t off = 0;
      while (off < take) {
        if (InterlockedCompareExchange(&slot->tx_stop, 0, 0)) return 0;
        DWORD written = 0;
        if (!WriteFile(hw, chunk + off, (DWORD)(take - off), &written, NULL) || written == 0) {
          InterlockedExchange(&slot->tx_failed, 1);
          return 1;
        }
        off += (size_t)written;
      }
    }
  }
}

static wchar_t* nm_utf8_to_wide(const char* s, int32_t len) {
  if (!s || len <= 0) return NULL;
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, NULL, 0);
  if (wlen <= 0) return NULL;
  wchar_t* w = (wchar_t*)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, w, wlen) != wlen) {
    free(w);
    return NULL;
  }
  w[wlen] = L'\0';
  return w;
}
#endif

#ifdef _WIN32
static void nm_finish_win_slot(FvNmSlot* slot) {
  if (slot->tx_thread) {
    CloseHandle(slot->tx_thread);
    slot->tx_thread = NULL;
  }
  if (slot->h_stdin_w && slot->h_stdin_w != INVALID_HANDLE_VALUE) {
    CloseHandle(slot->h_stdin_w);
    slot->h_stdin_w = NULL;
  }
  if (slot->tx_event) {
    CloseHandle(slot->tx_event);
    slot->tx_event = NULL;
  }
  if (slot->tx_sync_init) {
    DeleteCriticalSection(&slot->tx_lock);
    slot->tx_sync_init = 0;
  }
  nm_reset_rx(slot);
  nm_reset_tx(slot);
  slot->gen = 0;
  slot->zombie = 0;
  slot->in_use = 0;
}

static void nm_reap_zombies(void) {
  for (int i = 0; i < FV_NM_MAX_PORTS; ++i) {
    FvNmSlot* s = &g_nm_slots[i];
    if (!s->in_use || !s->zombie) continue;
    if (!s->tx_thread || WaitForSingleObject(s->tx_thread, 0) == WAIT_OBJECT_0) {
      nm_finish_win_slot(s);
    } else {
      (void)CancelSynchronousIo(s->tx_thread);
      if (s->tx_event) SetEvent(s->tx_event);
    }
  }
}
#endif

static void nm_close_slot(FvNmSlot* slot) {
  if (!slot || !slot->in_use) return;
#ifdef _WIN32
  if (slot->zombie) {
    if (!slot->tx_thread || WaitForSingleObject(slot->tx_thread, 0) == WAIT_OBJECT_0) {
      nm_finish_win_slot(slot);
    }
    return;
  }
  /* Stop submitting writes and invalidate handle immediately. */
  slot->gen = 0;
  if (slot->tx_sync_init) {
    InterlockedExchange(&slot->tx_stop, 1);
    if (slot->tx_event) SetEvent(slot->tx_event);
  }
  /* End child process so its stdin pipe breaks cleanly, and cancel any blocked WriteFile. */
  if (slot->h_stdout_r && slot->h_stdout_r != INVALID_HANDLE_VALUE) {
    CloseHandle(slot->h_stdout_r);
    slot->h_stdout_r = NULL;
  }
  if (slot->h_proc && slot->h_proc != INVALID_HANDLE_VALUE) {
    if (WaitForSingleObject(slot->h_proc, 50) == WAIT_TIMEOUT) {
      TerminateProcess(slot->h_proc, 1);
      WaitForSingleObject(slot->h_proc, 200);
    }
    CloseHandle(slot->h_proc);
    slot->h_proc = NULL;
  }
  int writer_exited = 1;
  if (slot->tx_thread) {
    (void)CancelSynchronousIo(slot->tx_thread);
    if (slot->tx_event) SetEvent(slot->tx_event);
    DWORD wres = WaitForSingleObject(slot->tx_thread, 2000);
    if (wres != WAIT_OBJECT_0) {
      (void)CancelSynchronousIo(slot->tx_thread);
      wres = WaitForSingleObject(slot->tx_thread, 1000);
    }
    if (wres != WAIT_OBJECT_0) {
      writer_exited = 0;
    }
  }
  if (!writer_exited) {
    /* Quarantine slot until nm_reap_zombies observes writer thread exit (zero UAF, bounded lifetime). */
    slot->zombie = 1;
    nm_reset_rx(slot);
    return;
  }
  nm_finish_win_slot(slot);
  return;
#else
  if (slot->fd_stdin_w >= 0) {
    close(slot->fd_stdin_w);
    slot->fd_stdin_w = -1;
  }
  if (slot->fd_stdout_r >= 0) {
    close(slot->fd_stdout_r);
    slot->fd_stdout_r = -1;
  }
  if (slot->pid > 0) {
    int st = 0;
    pid_t r = 0;
    do {
      r = waitpid(slot->pid, &st, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
      (void)kill(slot->pid, SIGTERM);
      int tries = 0;
      while (tries < 20) {
        do {
          r = waitpid(slot->pid, &st, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r != 0) break;
        usleep(5000);
        tries++;
      }
      if (r == 0) {
        (void)kill(slot->pid, SIGKILL);
        do {
          r = waitpid(slot->pid, &st, 0);
        } while (r < 0 && errno == EINTR);
      }
    }
    slot->pid = -1;
  }
#endif
  nm_reset_rx(slot);
  nm_reset_tx(slot);
  slot->gen = 0;
  slot->in_use = 0;
}

void fv_nm_close_all(void) {
  for (int i = 0; i < FV_NM_MAX_PORTS; ++i) {
    if (g_nm_slots[i].in_use) {
      nm_close_slot(&g_nm_slots[i]);
    }
  }
}

#if !defined(_WIN32) && !defined(__APPLE__)
/* Strictly async-signal-safe fd closer for post-fork child (no heap/DIR* calls). */
static void nm_close_extra_fds(void) {
#if defined(__linux__) && defined(__NR_close_range)
  if (syscall(__NR_close_range, 3U, ~0U, 0U) == 0) {
    return;
  }
#endif
  long max_fd = 1024;
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
    max_fd = (long)rl.rlim_cur;
  } else {
    long sc = sysconf(_SC_OPEN_MAX);
    if (sc > 0) max_fd = sc;
  }
  if (max_fd < 3) max_fd = 1024;
  if (max_fd > 65536) max_fd = 65536;
  for (int fd = 3; fd < (int)max_fd; ++fd) {
    close(fd);
  }
}

static void nm_reset_child_signals(void) {
  sigset_t empty;
  sigemptyset(&empty);
  (void)sigprocmask(SIG_SETMASK, &empty, NULL);
  const int sigs[] = { SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD, SIGALRM };
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
    signal(sigs[i], SIG_DFL);
  }
}
#endif

int32_t fv_nm_spawn(const char* exe_path, int32_t exe_len,
                    const char* origin, int32_t origin_len) {
  if (!exe_path || exe_len <= 0) return -1;
  if (!g_nm_atexit_registered) {
    g_nm_atexit_registered = 1;
    atexit(fv_nm_close_all);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
  }
#ifdef _WIN32
  nm_reap_zombies();
#endif
  int idx = -1;
  for (int i = 0; i < FV_NM_MAX_PORTS; ++i) {
    if (!g_nm_slots[i].in_use) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return -2;

  FvNmSlot* slot = &g_nm_slots[idx];
  memset(slot, 0, sizeof(*slot));

#ifdef _WIN32
  wchar_t* wexe = nm_utf8_to_wide(exe_path, exe_len);
  if (!wexe) return -3;
  wchar_t* worig = (origin && origin_len > 0) ? nm_utf8_to_wide(origin, origin_len) : NULL;

  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
  if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    free(wexe);
    free(worig);
    return -4;
  }
  if (!SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(out_w);
    free(wexe);
    free(worig);
    return -4;
  }

  STARTUPINFOEXW siex;
  PROCESS_INFORMATION pi;
  memset(&siex, 0, sizeof(siex));
  memset(&pi, 0, sizeof(pi));
  siex.StartupInfo.cb = sizeof(siex);
  siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  siex.StartupInfo.hStdInput = in_r;
  siex.StartupInfo.hStdOutput = out_w;
  siex.StartupInfo.hStdError = NULL;

  HANDLE inherit_list[2] = { in_r, out_w };
  SIZE_T attr_size = 0;
  (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list = attr_size > 0 ? (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size) : NULL;
  int attr_ok = 0;
  if (attr_list && InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
    if (UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherit_list, sizeof(inherit_list), NULL, NULL)) {
      siex.lpAttributeList = attr_list;
      attr_ok = 1;
    } else {
      DeleteProcThreadAttributeList(attr_list);
    }
  }
  if (!attr_ok) {
    free(attr_list);
    CloseHandle(in_r);
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(out_w);
    free(wexe);
    free(worig);
    return -5;
  }

  size_t cmd_cap = wcslen(wexe) + (worig ? wcslen(worig) : 0) + 16;
  wchar_t* cmd = (wchar_t*)malloc(cmd_cap * sizeof(wchar_t));
  if (!cmd) {
    DeleteProcThreadAttributeList(attr_list);
    free(attr_list);
    CloseHandle(in_r);
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(out_w);
    free(wexe);
    free(worig);
    return -5;
  }
  if (worig && worig[0] != L'\0') {
    swprintf(cmd, cmd_cap, L"\"%ls\" \"%ls\"", wexe, worig);
  } else {
    swprintf(cmd, cmd_cap, L"\"%ls\"", wexe);
  }
  BOOL ok = CreateProcessW(wexe, cmd, NULL, NULL, TRUE,
                           CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                           NULL, NULL, &siex.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attr_list);
  free(attr_list);
  free(cmd);
  CloseHandle(in_r);
  CloseHandle(out_w);
  free(wexe);
  free(worig);
  if (!ok) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    return -6;
  }
  CloseHandle(pi.hThread);

  InitializeCriticalSection(&slot->tx_lock);
  slot->tx_sync_init = 1;
  slot->tx_event = CreateEventW(NULL, FALSE, FALSE, NULL);
  slot->h_proc = pi.hProcess;
  slot->h_stdin_w = in_w;
  slot->h_stdout_r = out_r;
  slot->tx_stop = 0;
  slot->tx_failed = 0;
  slot->tx_thread = CreateThread(NULL, 0, nm_win_writer_proc, slot, 0, NULL);
  if (!slot->tx_event || !slot->tx_thread) {
    slot->in_use = 1;
    slot->gen = 1;
    nm_close_slot(slot);
    return -7;
  }
  uint32_t gen = g_nm_gen_seq++;
  if (gen == 0 || gen > 0x7FFFFFu) {
    g_nm_gen_seq = 2;
    gen = 1;
  }
  slot->gen = gen;
  slot->in_use = 1;
  return nm_make_handle(idx, gen);
#else
  char* exe = nm_dup_str(exe_path, exe_len);
  char* orig = (origin && origin_len > 0) ? nm_dup_str(origin, origin_len) : NULL;
  if (!exe) {
    free(orig);
    return -3;
  }

  slot->pid = -1;
  slot->fd_stdin_w = -1;
  slot->fd_stdout_r = -1;

  if (access(exe, X_OK) != 0) {
    free(exe);
    free(orig);
    return -4;
  }

  int p_in[2] = {-1, -1};
  int p_out[2] = {-1, -1};
  if (pipe(p_in) != 0) {
    free(exe);
    free(orig);
    return -5;
  }
  if (pipe(p_out) != 0) {
    close(p_in[0]);
    close(p_in[1]);
    free(exe);
    free(orig);
    return -5;
  }

  pid_t pid = -1;
#ifdef __APPLE__
  /* macOS: posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT atomically closes all non-stdio fds without fork(). */
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  if (posix_spawn_file_actions_init(&fa) != 0 || posix_spawnattr_init(&attr) != 0) {
    close(p_in[0]);
    close(p_in[1]);
    close(p_out[0]);
    close(p_out[1]);
    free(exe);
    free(orig);
    return -6;
  }
  (void)posix_spawn_file_actions_adddup2(&fa, p_in[0], STDIN_FILENO);
  (void)posix_spawn_file_actions_adddup2(&fa, p_out[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_addinherit_np(&fa, STDERR_FILENO);
  sigset_t empty_mask, def_sigs;
  sigemptyset(&empty_mask);
  sigemptyset(&def_sigs);
  sigaddset(&def_sigs, SIGPIPE);
  sigaddset(&def_sigs, SIGINT);
  sigaddset(&def_sigs, SIGTERM);
  sigaddset(&def_sigs, SIGHUP);
  sigaddset(&def_sigs, SIGQUIT);
  sigaddset(&def_sigs, SIGCHLD);
  sigaddset(&def_sigs, SIGALRM);
  (void)posix_spawnattr_setsigmask(&attr, &empty_mask);
  (void)posix_spawnattr_setsigdefault(&attr, &def_sigs);
  short sflags = POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  (void)posix_spawnattr_setflags(&attr, sflags);
  char* argv[3];
  argv[0] = exe;
  argv[1] = (orig && orig[0]) ? orig : NULL;
  argv[2] = NULL;
  int sp_rc = posix_spawn(&pid, exe, &fa, &attr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (sp_rc != 0) {
    close(p_in[0]);
    close(p_in[1]);
    close(p_out[0]);
    close(p_out[1]);
    free(exe);
    free(orig);
    return -6;
  }
#else
  pid = fork();
  if (pid < 0) {
    close(p_in[0]);
    close(p_in[1]);
    close(p_out[0]);
    close(p_out[1]);
    free(exe);
    free(orig);
    return -6;
  }
  if (pid == 0) {
    (void)dup2(p_in[0], STDIN_FILENO);
    (void)dup2(p_out[1], STDOUT_FILENO);
    nm_close_extra_fds();
    nm_reset_child_signals();
    char* argv[3];
    argv[0] = exe;
    argv[1] = (orig && orig[0]) ? orig : NULL;
    argv[2] = NULL;
    execv(exe, argv);
    _exit(127);
  }
#endif

  close(p_in[0]);
  close(p_out[1]);
  free(exe);
  free(orig);

  int fl_out = fcntl(p_out[0], F_GETFL, 0);
  if (fl_out >= 0) (void)fcntl(p_out[0], F_SETFL, fl_out | O_NONBLOCK);
  int fl_in = fcntl(p_in[1], F_GETFL, 0);
  if (fl_in >= 0) (void)fcntl(p_in[1], F_SETFL, fl_in | O_NONBLOCK);
  (void)fcntl(p_in[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(p_out[0], F_SETFD, FD_CLOEXEC);

  uint32_t gen = g_nm_gen_seq++;
  if (gen == 0 || gen > 0x7FFFFFu) {
    g_nm_gen_seq = 2;
    gen = 1;
  }
  slot->pid = pid;
  slot->fd_stdin_w = p_in[1];
  slot->fd_stdout_r = p_out[0];
  slot->gen = gen;
  slot->in_use = 1;
  return nm_make_handle(idx, gen);
#endif
}

// Non-blocking flush of pending outgoing bytes. Returns 1 on ok, 0 on fatal pipe error.
static int nm_flush_tx(FvNmSlot* slot) {
#ifdef _WIN32
  if (InterlockedCompareExchange(&slot->tx_failed, 0, 0) != 0) {
    return 0;
  }
  return 1;
#else
  while (slot->tx_off < slot->tx_len) {
    ssize_t nw = write(slot->fd_stdin_w, slot->tx_buf + slot->tx_off, slot->tx_len - slot->tx_off);
    if (nw > 0) {
      slot->tx_off += (size_t)nw;
      continue;
    }
    if (nw < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return 0;
    }
    return 0;
  }
  if (slot->tx_off == slot->tx_len) {
    slot->tx_off = 0;
    slot->tx_len = 0;
  } else if (slot->tx_off > 65536 && slot->tx_off * 2 >= slot->tx_len) {
    size_t rem = slot->tx_len - slot->tx_off;
    memmove(slot->tx_buf, slot->tx_buf + slot->tx_off, rem);
    slot->tx_off = 0;
    slot->tx_len = rem;
  }
  return 1;
#endif
}

static int nm_enqueue_tx(FvNmSlot* slot, const uint8_t* hdr, const uint8_t* body, size_t body_len) {
  size_t total_add = 4 + body_len;
#ifdef _WIN32
  EnterCriticalSection(&slot->tx_lock);
  if (InterlockedCompareExchange(&slot->tx_failed, 0, 0) != 0) {
    LeaveCriticalSection(&slot->tx_lock);
    return 0;
  }
#endif
  size_t pending = slot->tx_len - slot->tx_off;
  if (pending + total_add > (size_t)FV_NM_MAX_OUTGOING_BYTES) {
#ifdef _WIN32
    LeaveCriticalSection(&slot->tx_lock);
#endif
    return 0;
  }
  if (slot->tx_off > 0 && slot->tx_len + total_add > slot->tx_cap) {
    memmove(slot->tx_buf, slot->tx_buf + slot->tx_off, pending);
    slot->tx_off = 0;
    slot->tx_len = pending;
  }
  if (slot->tx_len + total_add > slot->tx_cap) {
    size_t new_cap = slot->tx_cap ? slot->tx_cap * 2 : 4096;
    while (new_cap < slot->tx_len + total_add) new_cap *= 2;
    if (new_cap > (size_t)FV_NM_MAX_OUTGOING_BYTES + 16) {
      new_cap = (size_t)FV_NM_MAX_OUTGOING_BYTES + 16;
    }
    uint8_t* nb = (uint8_t*)realloc(slot->tx_buf, new_cap);
    if (!nb) {
#ifdef _WIN32
      LeaveCriticalSection(&slot->tx_lock);
#endif
      return 0;
    }
    slot->tx_buf = nb;
    slot->tx_cap = new_cap;
  }
  memcpy(slot->tx_buf + slot->tx_len, hdr, 4);
  memcpy(slot->tx_buf + slot->tx_len + 4, body, body_len);
  slot->tx_len += total_add;
#ifdef _WIN32
  LeaveCriticalSection(&slot->tx_lock);
  if (slot->tx_event) SetEvent(slot->tx_event);
  return 1;
#else
  return nm_flush_tx(slot);
#endif
}

int32_t fv_nm_write(int32_t handle, const uint8_t* json_utf8, int32_t json_len) {
  FvNmSlot* slot = nm_lookup(handle);
  if (!slot) return 0;
  if (!json_utf8 || json_len <= 0 || json_len > FV_NM_MAX_OUTGOING_BYTES - 4) return 0;
  uint32_t n = (uint32_t)json_len;
  uint8_t hdr[4];
  memcpy(hdr, &n, 4);
  if (!nm_enqueue_tx(slot, hdr, json_utf8, (size_t)json_len)) {
    nm_close_slot(slot);
    return 0;
  }
  return 1;
}

// Returns >0 bytes read, 0 if EAGAIN/no data available now, -1 on EOF/error.
static int32_t nm_read_nonblock(FvNmSlot* slot, uint8_t* dst, size_t want) {
  if (want == 0) return 0;
#ifdef _WIN32
  DWORD avail = 0;
  if (!PeekNamedPipe(slot->h_stdout_r, NULL, 0, NULL, &avail, NULL)) {
    return -1;
  }
  if (avail == 0) {
    if (WaitForSingleObject(slot->h_proc, 0) == WAIT_OBJECT_0) {
      return -1;
    }
    return 0;
  }
  DWORD to_read = (DWORD)(want < (size_t)avail ? want : (size_t)avail);
  DWORD got = 0;
  if (!ReadFile(slot->h_stdout_r, dst, to_read, &got, NULL)) {
    return -1;
  }
  return (int32_t)got;
#else
  for (;;) {
    ssize_t nr = read(slot->fd_stdout_r, dst, want);
    if (nr > 0) return (int32_t)nr;
    if (nr == 0) return -1;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
#endif
}

static int32_t nm_pump_rx(FvNmSlot* slot) {
  if (!nm_flush_tx(slot)) {
    nm_close_slot(slot);
    return -1;
  }
  if (slot->msg_ready) {
    return (int32_t)slot->msg_len;
  }
  while (slot->len_got < 4) {
    int32_t nr = nm_read_nonblock(slot, slot->len_buf + slot->len_got, (size_t)(4 - slot->len_got));
    if (nr < 0) {
      nm_close_slot(slot);
      return -1;
    }
    if (nr == 0) return 0;
    slot->len_got += nr;
    if (slot->len_got == 4) {
      uint32_t mlen = 0;
      memcpy(&mlen, slot->len_buf, 4);
      if (mlen == 0 || mlen > (uint32_t)FV_NM_MAX_INCOMING_BYTES) {
        nm_close_slot(slot);
        return -1;
      }
      slot->msg_len = mlen;
      slot->msg_buf = (uint8_t*)malloc(mlen);
      slot->msg_got = 0;
      if (!slot->msg_buf) {
        nm_close_slot(slot);
        return -1;
      }
    }
  }
  while (slot->msg_got < slot->msg_len) {
    int32_t nr = nm_read_nonblock(slot, slot->msg_buf + slot->msg_got, (size_t)(slot->msg_len - slot->msg_got));
    if (nr < 0) {
      nm_close_slot(slot);
      return -1;
    }
    if (nr == 0) return 0;
    slot->msg_got += (uint32_t)nr;
  }
  slot->msg_ready = 1;
  return (int32_t)slot->msg_len;
}

int32_t fv_nm_peek_len(int32_t handle) {
  FvNmSlot* slot = nm_lookup(handle);
  if (!slot) return -1;
  return nm_pump_rx(slot);
}

int32_t fv_nm_poll(int32_t handle, uint8_t* out_buf, int32_t out_cap) {
  FvNmSlot* slot = nm_lookup(handle);
  if (!slot) return -1;
  int32_t ready = nm_pump_rx(slot);
  if (ready <= 0) return ready;
  if (!out_buf || out_cap < ready) {
    // Retain the assembled frame so the caller can retry with a buffer of size `ready`.
    return -2;
  }
  memcpy(out_buf, slot->msg_buf, (size_t)ready);
  nm_reset_rx(slot);
  return ready;
}

void fv_nm_close(int32_t handle) {
#ifdef _WIN32
  nm_reap_zombies();
#endif
  FvNmSlot* slot = nm_lookup(handle);
  if (!slot) return;
  nm_close_slot(slot);
}

int32_t fv_nm_active_count(void) {
#ifdef _WIN32
  nm_reap_zombies();
#endif
  int32_t c = 0;
  for (int i = 0; i < FV_NM_MAX_PORTS; ++i) {
#ifdef _WIN32
    if (g_nm_slots[i].in_use && !g_nm_slots[i].zombie) c++;
#else
    if (g_nm_slots[i].in_use) c++;
#endif
  }
  return c;
}

int64_t fv_nm_epoch_ms(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = (((uint64_t)ft.dwHighDateTime) << 32) | (uint64_t)ft.dwLowDateTime;
  // Convert 100-ns intervals since 1601-01-01 to ms since 1970-01-01
  const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  if (t < EPOCH_DIFF_100NS) return 0;
  return (int64_t)((t - EPOCH_DIFF_100NS) / 10000ULL);
#else
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) return 0;
  return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
#endif
}

int32_t fv_nm_lookup_win_registry(const char* app_name, int32_t app_len,
                                  uint8_t* out_buf, int32_t out_cap) {
#ifdef _WIN32
  if (!app_name || app_len <= 0 || !out_buf || out_cap <= 0) return -1;
  wchar_t* wapp = nm_utf8_to_wide(app_name, app_len);
  if (!wapp) return -1;
  static const wchar_t* prefixes[] = {
    L"SOFTWARE\\Google\\Chrome\\NativeMessagingHosts\\",
    L"SOFTWARE\\Chromium\\NativeMessagingHosts\\",
    L"SOFTWARE\\Microsoft\\Edge\\NativeMessagingHosts\\",
  };
  static const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
  wchar_t val_w[2048];
  for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p) {
      wchar_t sub[512];
      int nw = swprintf(sub, 512, L"%ls%ls", prefixes[p], wapp);
      if (nw <= 0 || nw >= 512) continue;
      DWORD cb = sizeof(val_w);
      LONG rc = RegGetValueW(roots[r], sub, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, val_w, &cb);
      if (rc == ERROR_SUCCESS && val_w[0] != L'\0') {
        int u8 = WideCharToMultiByte(CP_UTF8, 0, val_w, -1, NULL, 0, NULL, NULL);
        free(wapp);
        if (u8 <= 1) return 0;
        if (u8 - 1 > out_cap) return -2;
        int written = WideCharToMultiByte(CP_UTF8, 0, val_w, -1, (char*)out_buf, out_cap, NULL, NULL);
        return (written > 1) ? (written - 1) : 0;
      }
    }
  }
  free(wapp);
  return 0;
#else
  (void)app_name;
  (void)app_len;
  (void)out_buf;
  (void)out_cap;
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// Cross-platform recursive unpacked extension file walker (symlink-safe)
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t* buf;
  int32_t cap;
  int32_t len;
  int32_t count;
  int32_t max_files;
  int truncated;
} FvWalkState;

static void walk_append(FvWalkState* st, const char* rel) {
  if (st->truncated) return;
  if (st->count >= st->max_files) {
    st->truncated = 1;
    return;
  }
  size_t rlen = strlen(rel);
  if (rlen == 0) return;
  if (st->len + (int32_t)rlen + 1 > st->cap) {
    st->truncated = 1;
    return;
  }
  memcpy(st->buf + st->len, rel, rlen);
  st->len += (int32_t)rlen;
  st->buf[st->len++] = '\0';
  st->count++;
}

static void walk_recurse(FvWalkState* st, const char* abs_dir, const char* rel_prefix, int depth) {
  if (st->truncated) return;
  if (depth > 32) {
    st->truncated = 1;
    return;
  }
#ifdef _WIN32
  wchar_t* wdir = nm_utf8_to_wide(abs_dir, (int32_t)strlen(abs_dir));
  if (!wdir) {
    st->truncated = 1;
    return;
  }
  size_t wlen = wcslen(wdir);
  wchar_t* pat = (wchar_t*)malloc((wlen + 8) * sizeof(wchar_t));
  if (!pat) {
    free(wdir);
    st->truncated = 1;
    return;
  }
  swprintf(pat, wlen + 8, L"%ls\\*", wdir);
  free(wdir);
  WIN32_FIND_DATAW ffd;
  HANDLE h = FindFirstFileW(pat, &ffd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (st->truncated) break;
    if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
    char name_u8[1024];
    int nu = WideCharToMultiByte(CP_UTF8, 0, ffd.cFileName, -1, name_u8, (int)sizeof(name_u8), NULL, NULL);
    if (nu <= 1 || nu >= (int)sizeof(name_u8)) {
      st->truncated = 1;
      break;
    }
    char child_abs[4096];
    char child_rel[4096];
    int n1 = snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_dir, name_u8);
    int n2 = (rel_prefix && rel_prefix[0])
                 ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, name_u8)
                 : snprintf(child_rel, sizeof(child_rel), "%s", name_u8);
    if (n1 < 0 || (size_t)n1 >= sizeof(child_abs) || n2 < 0 || (size_t)n2 >= sizeof(child_rel)) {
      st->truncated = 1;
      break;
    }
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      walk_recurse(st, child_abs, child_rel, depth + 1);
    } else {
      walk_append(st, child_rel);
    }
  } while (FindNextFileW(h, &ffd));
  FindClose(h);
#else
  DIR* d = opendir(abs_dir);
  if (!d) return;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (st->truncated) break;
    const char* name = ent->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    char child_abs[4096];
    char child_rel[4096];
    int n1 = snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_dir, name);
    int n2 = (rel_prefix && rel_prefix[0])
                 ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, name)
                 : snprintf(child_rel, sizeof(child_rel), "%s", name);
    if (n1 < 0 || (size_t)n1 >= sizeof(child_abs) || n2 < 0 || (size_t)n2 >= sizeof(child_rel)) {
      st->truncated = 1;
      break;
    }
    struct stat sb;
    if (lstat(child_abs, &sb) != 0) continue;
    if (S_ISLNK(sb.st_mode)) continue;
    if (S_ISDIR(sb.st_mode)) {
      walk_recurse(st, child_abs, child_rel, depth + 1);
    } else if (S_ISREG(sb.st_mode)) {
      walk_append(st, child_rel);
    }
  }
  closedir(d);
#endif
}

int32_t fv_ext_walk_dir(const char* root_dir, int32_t root_len,
                        uint8_t* out_buf, int32_t out_cap, int32_t max_files) {
  if (!root_dir || root_len <= 0 || !out_buf || out_cap <= 0 || max_files <= 0) return -1;
  char* root = nm_dup_str(root_dir, root_len);
  if (!root) return -1;
  while (root_len > 1 && (root[root_len - 1] == '/' || root[root_len - 1] == '\\')) {
    root[--root_len] = '\0';
  }
  FvWalkState st;
  st.buf = out_buf;
  st.cap = out_cap;
  st.len = 0;
  st.count = 0;
  st.max_files = max_files;
  st.truncated = 0;
  walk_recurse(&st, root, "", 0);
  free(root);
  if (st.truncated) return -2;
  return st.len;
}
