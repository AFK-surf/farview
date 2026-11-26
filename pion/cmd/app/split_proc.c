// Process-isolated secondary `pion-host` lifecycle manager for Arc Split View.
// Uses posix_spawn (POSIX) or CreateProcessA (Windows) + exact-PID termination/reaping
// to guarantee zero zombie processes.

#include "../../../native/win_compat.h"

#ifdef _WIN32
#include <shellapi.h>
#define FV_PATH_SEP '\\'
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <malloc/malloc.h>
#include <sys/xattr.h>
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define FV_PATH_SEP '/'
#endif

// ---------------------------------------------------------------------------------------------------------------
// Shared by both platforms (and called from the app's unit tests, so they run on every OS).
// ---------------------------------------------------------------------------------------------------------------

/* 1 when this client runs on Windows (its paths are "X:\\..." / "\\\\server\\..."), else 0. */
int32_t fv_app_client_is_windows(void) {
#ifdef _WIN32
  return 1;
#else
  return 0;
#endif
}

/* Per-frame Objective-C autorelease pool guard on macOS: drains autoreleased Foundation/ImageIO
 * buffers (such as NSData created by image uploads) every frame instead of retaining them until exit. */
int64_t fv_app_frame_pool_push(void) {
#ifdef __APPLE__
  return (int64_t)(intptr_t)objc_autoreleasePoolPush();
#else
  return 0;
#endif
}

void fv_app_frame_pool_pop(int64_t pool) {
#ifdef __APPLE__
  if (pool != 0) objc_autoreleasePoolPop((void *)(intptr_t)pool);
#else
  (void)pool;
#endif
}

/* Ask the platform allocator to return idle large pages back to the OS. */
void fv_app_malloc_trim(void) {
#ifdef __APPLE__
  malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
  (void)malloc_trim(0);
#endif
}

static int fv_is_sep(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

/* Whether `p` is an absolute path: "/..." on POSIX; "X:\..." / "X:/..." or "\\server\..." on Windows. A relative
 * one is refused before it reaches an opener (it would be looked up beside the app, or read as an option). */
static int fv_path_is_absolute(const char *p) {
  if (!p || !p[0]) return 0;
#ifdef _WIN32
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && fv_is_sep(p[2])) return 1;
  return fv_is_sep(p[0]) && fv_is_sep(p[1]);
#else
  return p[0] == '/';
#endif
}

/* `~`, `~/...` (and `~\...` on Windows) under the person's home: USERPROFILE, else HOME, on Windows; HOME on POSIX.
 * The one resolver every path from Pion goes through (writer, reader, opener, thumbnails), so all agree. */
static int fv_resolve_home_path(const char *path_ptr, int32_t path_len, char *out, size_t out_cap) {
  if (!path_ptr || path_len <= 0 || (size_t)path_len >= out_cap) return -1;
  char raw[4096];
  if ((size_t)path_len >= sizeof(raw)) return -1;
  memcpy(raw, path_ptr, (size_t)path_len);
  raw[path_len] = '\0';
  if (raw[0] == '~' && (raw[1] == '\0' || fv_is_sep(raw[1]))) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char *home = getenv("HOME");
#endif
    if (!home || !home[0]) return -1;
    size_t hlen = strlen(home);
    size_t rlen = strlen(raw + 1);
    if (hlen + rlen >= out_cap) return -1;
    memcpy(out, home, hlen);
    memcpy(out + hlen, raw + 1, rlen + 1);
  } else {
    memcpy(out, raw, (size_t)path_len + 1);
  }
  return 0;
}

/* `path` with `~` resolved as above into `out`: its length, -1 when it cannot be (thumbnails read the file the
 * downloads worker wrote, at the same place). */
int32_t fv_app_expand_home(const char *path_ptr, int32_t path_len, char *out, int32_t out_cap) {
  if (!out || out_cap <= 0) return -1;
  if (path_ptr && path_len > 0 && memchr(path_ptr, 0, (size_t)path_len)) return -1;
  if (fv_resolve_home_path(path_ptr, path_len, out, (size_t)out_cap) != 0) return -1;
  return (int32_t)strlen(out);
}

/* Makes every missing folder above the file `path` (mkdir -p of its parent); failures are left to the open that
 * follows. */
static void fv_mkdirs_parent(const char *path) {
  char buf[4096];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) return;
  memcpy(buf, path, n + 1);
  for (size_t i = 1; i < n; i++) {
    if (!fv_is_sep(buf[i]) || fv_is_sep(buf[i - 1])) continue;
    char c = buf[i];
    buf[i] = '\0';
    (void)mkdir(buf, 0755);
    buf[i] = c;
  }
}

/* Whether `path` may be handed to the system opener as a file: an absolute path to a regular file itself (not a
 * directory, not a symlink, not a junction or other reparse point). */
static int fv_open_target_is_file(const char *path) {
  if (!fv_path_is_absolute(path)) return 0;
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
  if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return 0;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
#else
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
#endif
  return 1;
}

/* Whether a file may be handed to the default app: its extension names a picture, video, sound, PDF or plain text
 * (the same list as shell_ui::library_opens_directly). Anything else (apps, scripts, .command, installers, archives,
 * web pages, no extension) is refused, so a downloaded program is never run by one click. */
static int fv_open_ext_allowed(const char *path) {
  static const char *const ok[] = { "png", "jpg", "jpeg", "gif", "webp", "heic", "avif", "bmp", "tiff", "mp4", "mov",
    "webm", "mkv", "m4v", "mp3", "wav", "m4a", "aac", "flac", "ogg", "opus", "pdf", "txt", "md", "csv", "json", "rtf",
    "epub", NULL };
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (fv_is_sep(*p)) base = p + 1;
  }
  const char *dot = strrchr(base, '.');
  if (!dot || dot == base || dot[1] == 0) return 0;
  char ext[8];
  size_t n = strlen(dot + 1);
  if (n >= sizeof(ext)) return 0;
  for (size_t i = 0; i <= n; i++) {
    char c = dot[1 + i];
    ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  for (int i = 0; ok[i]; i++) {
    if (strcmp(ext, ok[i]) == 0) return 1;
  }
  return 0;
}

/* 1 when `fv_app_open_file` would hand `path` (may start with ~/) to the opener: an absolute path to a regular
 * file (no directory, no symlink) whose extension is one above; else 0. */
int32_t fv_app_open_target_ok(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (!path_ptr || path_len <= 0 || memchr(path_ptr, 0, (size_t)path_len)) return 0;
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  return fv_open_target_is_file(resolved) && fv_open_ext_allowed(resolved) ? 1 : 0;
}

/* Appends `in` (`in_len` bytes) to `out` as one argument of a Windows command line, quoted so that
 * CommandLineToArgvW and the MSVC runtime read it back exactly: always in quotes, a quote as \", and the backslashes
 * before a quote (or the closing quote) doubled. The length written, -1 when `cap` is too small. */
int32_t fv_app_win_quote_arg(const char *in, int32_t in_len, char *out, int32_t cap) {
  if ((!in && in_len > 0) || in_len < 0 || !out || cap <= 0) return -1;
  int32_t o = 0;
#define FV_Q_PUT(ch) \
  do { \
    if (o + 1 >= cap) return -1; \
    out[o++] = (ch); \
  } while (0)
  FV_Q_PUT('"');
  int32_t i = 0;
  for (;;) {
    int32_t slashes = 0;
    while (i < in_len && in[i] == '\\') {
      slashes++;
      i++;
    }
    if (i >= in_len) {
      for (int32_t k = 0; k < slashes * 2; k++) FV_Q_PUT('\\');
      break;
    }
    if (in[i] == '"') {
      for (int32_t k = 0; k < slashes * 2 + 1; k++) FV_Q_PUT('\\');
      FV_Q_PUT('"');
    } else {
      for (int32_t k = 0; k < slashes; k++) FV_Q_PUT('\\');
      FV_Q_PUT(in[i]);
    }
    i++;
  }
  FV_Q_PUT('"');
#undef FV_Q_PUT
  out[o] = '\0';
  return o;
}

/* Whether the environment entry `kv` ("NAME=value") is kept from the split-pane host: its profile, state and storage
 * variables are its own (Windows names compare without case). 1 when dropped. */
int32_t fv_app_split_env_dropped(const char *kv) {
  static const char *const names[] = { "FV_PROFILE_DIR=", "FV_STATE_DIR=", "FV_STORAGE=", NULL };
  if (!kv) return 0;
  for (int i = 0; names[i]; i++) {
    size_t n = strlen(names[i]);
#ifdef _WIN32
    if (_strnicmp(kv, names[i], n) == 0) return 1;
#else
    if (strncmp(kv, names[i], n) == 0) return 1;
#endif
  }
  return 0;
}

// Copies file `from` over `to` (atomically, through `to`.tmp); 0 when copied, -1 when `from` is not there or
// a write failed (then `to` is left as it was).
static int fv_split_copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return -1;
  char tmp[PATH_MAX + 8];
  if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", to) >= sizeof(tmp)) {
    fclose(in);
    return -1;
  }
#ifdef _WIN32
  FILE *out = fopen(tmp, "wb");
#else
  int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  FILE *out = ofd >= 0 ? fdopen(ofd, "wb") : NULL;
  if (!out && ofd >= 0) close(ofd);
#endif
  if (!out) {
    fclose(in);
    return -1;
  }
  char buf[16384];
  int ok = 1;
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      ok = 0;
      break;
    }
  }
  if (ferror(in)) ok = 0;
  fclose(in);
  if (fclose(out) != 0) ok = 0;
#ifdef _WIN32
  if (ok && !MoveFileExA(tmp, to, MOVEFILE_REPLACE_EXISTING)) ok = 0;
#else
  if (ok && rename(tmp, to) != 0) ok = 0;
#endif
  if (!ok) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

// The split-pane host's own state directory, made here: <FV_STATE_DIR>/split-<port> beside the primary's state,
// else <temp>/farview-split-<uid>/split-<port> (<TEMP>\farview-split\split-<port> on Windows, a per-user folder).
// Seeded (each spawn) with copies of the primary's cookies.txt and storage.json, which the split host may change
// without ever touching the primary's. 0 when `out` holds it.
int32_t fv_split_state_dir(int port, char *out, size_t out_cap) {
  const char *primary = getenv("FV_STATE_DIR");
  char base[PATH_MAX];
  if (primary && primary[0] != '\0') {
    if ((size_t)snprintf(base, sizeof(base), "%s", primary) >= sizeof(base)) return -1;
  } else {
#ifdef _WIN32
    const char *tmpd = getenv("TEMP");
    if (!tmpd || tmpd[0] == '\0') tmpd = getenv("TMP");
    if (!tmpd || tmpd[0] == '\0') return -1;
#else
    const char *tmpd = getenv("TMPDIR");
    if (!tmpd || tmpd[0] == '\0') tmpd = "/tmp";
#endif
    size_t tl = strlen(tmpd);
    while (tl > 1 && fv_is_sep(tmpd[tl - 1])) tl--;
#ifdef _WIN32
    if ((size_t)snprintf(base, sizeof(base), "%.*s\\farview-split", (int)tl, tmpd) >= sizeof(base)) return -1;
#else
    if ((size_t)snprintf(base, sizeof(base), "%.*s/farview-split-%u", (int)tl, tmpd, (unsigned)getuid()) >= sizeof(base)) return -1;
#endif
  }
  if (mkdir(base, 0700) != 0 && errno != EEXIST) return -1;
  if ((size_t)snprintf(out, out_cap, "%s%csplit-%d", base, FV_PATH_SEP, port) >= out_cap) return -1;
  if (mkdir(out, 0700) != 0 && errno != EEXIST) return -1;
  if (primary && primary[0] != '\0') {
    const char *names[] = {"cookies.txt", "storage.json"};
    for (int i = 0; i < 2; i++) {
      char from[PATH_MAX];
      char to[PATH_MAX];
      if ((size_t)snprintf(from, sizeof(from), "%s%c%s", primary, FV_PATH_SEP, names[i]) >= sizeof(from)) continue;
      if ((size_t)snprintf(to, sizeof(to), "%s%c%s", out, FV_PATH_SEP, names[i]) >= sizeof(to)) continue;
      (void)fv_split_copy_file(from, to);
    }
  }
  return 0;
}

/* The state directory a split host on `port` is given, made and seeded as above, into `out`: its length, -1 when
 * it could not be made (tests). */
int32_t fv_app_split_state_dir(int32_t port, char *out, int32_t out_cap) {
  if (!out || out_cap <= 0) return -1;
  if (fv_split_state_dir((int)port, out, (size_t)out_cap) != 0) return -1;
  return (int32_t)strlen(out);
}

#ifdef _WIN32

typedef struct {
  DWORD pid;
  HANDLE handle;
  int killing;
} SplitWinEntry;

static pthread_mutex_t g_split_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_split_cond = PTHREAD_COND_INITIALIZER;
static SplitWinEntry *g_split_entries = NULL;
static int g_split_count = 0;
static int g_split_cap = 0;
static int g_atexit_registered = 0;
static int g_shutting_down = 0;

static int find_split_entry_locked(DWORD p) {
  for (int i = 0; i < g_split_count; i++) {
    if (g_split_entries[i].pid == p) return i;
  }
  return -1;
}

static int ensure_split_cap_locked(void) {
  if (g_split_count < g_split_cap) return 1;
  int next_cap = g_split_cap == 0 ? 16 : g_split_cap * 2;
  SplitWinEntry *next = (SplitWinEntry *)realloc(g_split_entries, (size_t)next_cap * sizeof(SplitWinEntry));
  if (!next) return 0;
  g_split_entries = next;
  g_split_cap = next_cap;
  return 1;
}

static void untrack_split_idx_locked(int idx) {
  if (idx >= 0 && idx < g_split_count) {
    HANDLE h = g_split_entries[idx].handle;
    g_split_entries[idx] = g_split_entries[--g_split_count];
    if (h) CloseHandle(h);
    pthread_cond_broadcast(&g_split_cond);
  }
}

int32_t fv_app_kill_split_host(int32_t pid) {
  if (pid <= 0) return 0;
  DWORD p = (DWORD)pid;
  pthread_mutex_lock(&g_split_mu);
  int idx = find_split_entry_locked(p);
  if (idx < 0) {
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }
  if (g_split_entries[idx].killing) {
    pthread_mutex_unlock(&g_split_mu);
    return 0;
  }
  g_split_entries[idx].killing = 1;
  HANDLE h = g_split_entries[idx].handle;
  pthread_mutex_unlock(&g_split_mu);

  int reaped = 0;
  if (h) {
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
      reaped = 1;
    } else {
      TerminateProcess(h, 1);
      if (WaitForSingleObject(h, 500) == WAIT_OBJECT_0) {
        reaped = 1;
      }
    }
  } else {
    reaped = 1;
  }

  pthread_mutex_lock(&g_split_mu);
  int ridx = find_split_entry_locked(p);
  if (ridx >= 0) {
    if (reaped) {
      untrack_split_idx_locked(ridx);
    } else {
      g_split_entries[ridx].killing = 0;
      pthread_cond_broadcast(&g_split_cond);
    }
  }
  pthread_mutex_unlock(&g_split_mu);
  return reaped ? 1 : 0;
}

void fv_app_cleanup_split_hosts(void) {
  pthread_mutex_lock(&g_split_mu);
  g_shutting_down = 1;
  pthread_cond_broadcast(&g_split_cond);
  int n = g_split_count;
  DWORD *snapshot = n > 0 ? (DWORD *)malloc((size_t)n * sizeof(DWORD)) : NULL;
  if (snapshot) {
    for (int i = 0; i < n; i++) snapshot[i] = g_split_entries[i].pid;
  }
  pthread_mutex_unlock(&g_split_mu);
  if (!snapshot) return;
  for (int i = 0; i < n; i++) {
    if (snapshot[i] > 0) (void)fv_app_kill_split_host((int32_t)snapshot[i]);
  }
  free(snapshot);
}

int32_t fv_app_reap_pid(int32_t pid) {
  if (pid <= 0) return 1;
  DWORD p = (DWORD)pid;
  pthread_mutex_lock(&g_split_mu);
  int idx = find_split_entry_locked(p);
  if (idx < 0) {
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }
  HANDLE h = g_split_entries[idx].handle;
  int reaped = (!h || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) ? 1 : 0;
  if (reaped) untrack_split_idx_locked(idx);
  pthread_mutex_unlock(&g_split_mu);
  return reaped;
}

int32_t fv_app_pick_free_port(void) {
  fv_win_wsa_init();
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return -1;
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(0);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
    closesocket(s);
    return -1;
  }
  socklen_t n = sizeof a;
  int port = -1;
  if (getsockname(s, (struct sockaddr *)&a, &n) == 0) {
    port = (int)ntohs(a.sin_port);
  }
  closesocket(s);
  return (int32_t)port;
}

static int resolve_host_binary(char *out, size_t out_sz) {
  const char *env_bin = getenv("FV_HOST_BIN");
  if (env_bin && env_bin[0] != '\0' && access(env_bin, F_OK) == 0) {
    snprintf(out, out_sz, "%s", env_bin);
    return 1;
  }
  char self_path[PATH_MAX];
  DWORD nr = GetModuleFileNameA(NULL, self_path, (DWORD)sizeof(self_path));
  if (nr > 0 && nr < (DWORD)sizeof(self_path)) {
    char *slash = strrchr(self_path, '\\');
    char *fslash = strrchr(self_path, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (slash) {
      *slash = '\0';
      snprintf(out, out_sz, "%s\\pion-host.exe", self_path);
      if (access(out, F_OK) == 0) return 1;
    }
  }
  if (access("./out/pion-host.exe", F_OK) == 0) {
    snprintf(out, out_sz, "%s", "./out/pion-host.exe");
    return 1;
  }
  snprintf(out, out_sz, "%s", "pion-host.exe");
  return 0;
}

int32_t fv_app_spawn_split_host(int32_t port, const void *url_ptr, int32_t url_len) {
  if (port <= 0) return -1;
  pthread_mutex_lock(&g_split_mu);
  if (g_shutting_down) {
    pthread_mutex_unlock(&g_split_mu);
    return -1;
  }
  if (!g_atexit_registered) {
    atexit(fv_app_cleanup_split_hosts);
    g_atexit_registered = 1;
  }
  pthread_mutex_unlock(&g_split_mu);

  char host_bin[PATH_MAX];
  (void)resolve_host_binary(host_bin, sizeof(host_bin));

  const char *fallback_url = "about:blank";
  char *url_buf = NULL;
  if (url_ptr != NULL && url_len > 0) {
    url_buf = (char *)malloc((size_t)url_len + 1);
    if (!url_buf) return -1;
    memcpy(url_buf, url_ptr, (size_t)url_len);
    url_buf[url_len] = '\0';
  }
  const char *u = url_buf ? url_buf : fallback_url;
  // Each argument quoted by the Win32 argv rules, so a URL holding quotes or backslashes stays one argument.
  size_t hl = strlen(host_bin);
  size_t ul = strlen(u);
  size_t cmd_cap = 2 * hl + 2 * ul + 64;
  char *cmd = (char *)malloc(cmd_cap);
  if (!cmd) {
    free(url_buf);
    return -1;
  }
  int32_t at = fv_app_win_quote_arg(host_bin, (int32_t)hl, cmd, (int32_t)cmd_cap);
  if (at >= 0) at += snprintf(cmd + at, cmd_cap - (size_t)at, " %d ", (int)port);
  int32_t ua = at >= 0 ? fv_app_win_quote_arg(u, (int32_t)ul, cmd + at, (int32_t)(cmd_cap - (size_t)at)) : -1;
  free(url_buf);
  if (ua < 0) {
    free(cmd);
    return -1;
  }

  // The secondary split-pane host never writes the primary browser's files (as on POSIX below): FV_PROFILE_DIR,
  // FV_STATE_DIR and FV_STORAGE are stripped, and FV_STATE_DIR names a state directory of its own seeded with a
  // copy of the primary's cookies and localStorage.
  char split_state[PATH_MAX];
  char split_state_env[PATH_MAX + 16];
  split_state_env[0] = '\0';
  if (fv_split_state_dir((int)port, split_state, sizeof(split_state)) == 0) {
    snprintf(split_state_env, sizeof(split_state_env), "FV_STATE_DIR=%s", split_state);
  }
  LPCH env_raw = GetEnvironmentStringsA();
  char *child_env = NULL;
  if (env_raw) {
    size_t total = 0;
    for (const char *p = env_raw; *p; p += strlen(p) + 1) {
      total += strlen(p) + 1;
    }
    total += strlen(split_state_env) + 1;
    child_env = (char *)malloc(total + 2);
    if (child_env) {
      char *dst = child_env;
      for (const char *p = env_raw; *p; p += strlen(p) + 1) {
        if (fv_app_split_env_dropped(p)) continue;
        size_t ln = strlen(p) + 1;
        memcpy(dst, p, ln);
        dst += ln;
      }
      if (split_state_env[0] != '\0') {
        size_t ln = strlen(split_state_env) + 1;
        memcpy(dst, split_state_env, ln);
        dst += ln;
      }
      *dst++ = '\0';
      if (dst == child_env + 1) *dst++ = '\0';
    }
    FreeEnvironmentStringsA(env_raw);
  }

  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  pthread_mutex_lock(&g_split_mu);
  if (g_shutting_down || !ensure_split_cap_locked()) {
    pthread_mutex_unlock(&g_split_mu);
    free(child_env);
    free(cmd);
    return -1;
  }
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, child_env, NULL, &si, &pi);
  DWORD pid = 0;
  if (ok) {
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;
    g_split_entries[g_split_count].pid = pid;
    g_split_entries[g_split_count].handle = pi.hProcess;
    g_split_entries[g_split_count].killing = 0;
    g_split_count++;
  }
  pthread_mutex_unlock(&g_split_mu);
  free(child_env);
  free(cmd);
  return ok ? (int32_t)pid : -1;
}

int32_t fv_app_verify_local_file(const char *path_ptr, int32_t path_len, int64_t expected_bytes) {
  char resolved[4096];
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  struct stat st;
  if (stat(resolved, &st) != 0) return 0;
  if (!S_ISREG(st.st_mode)) return 0;
  if (expected_bytes > 0 && (int64_t)st.st_size != expected_bytes) return 0;
  return 1;
}

int32_t fv_app_reveal_file(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  if (!fv_path_is_absolute(resolved) || access(resolved, F_OK) != 0) return -1;
  for (char *p = resolved; *p; p++) {
    if (*p == '/') *p = '\\';
  }
  char params[4200];
  snprintf(params, sizeof(params), "/select,\"%s\"", resolved);
  HINSTANCE h = ShellExecuteA(NULL, "open", "explorer.exe", params, NULL, SW_SHOWNORMAL);
  return ((intptr_t)h > 32) ? 0 : -1;
}

/* Windows forms of the download and open helpers below (client_downloads.pion, library opens): same contracts. */
int32_t fv_app_open_file(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (!fv_app_open_target_ok(path_ptr, path_len)) return -1;
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  HINSTANCE h = ShellExecuteA(NULL, "open", resolved, NULL, NULL, SW_SHOWNORMAL);
  return ((intptr_t)h > 32) ? 0 : -1;
}

int64_t fv_app_bytes_addr(const void *p) { return (int64_t)(intptr_t)p; }

static int fv_win_dl_resolve(const char *path_ptr, int32_t path_len, char *out, size_t cap) {
  if (!path_ptr || path_len <= 0) return -1;
  if (memchr(path_ptr, 0, (size_t)path_len)) return -1;
  return fv_resolve_home_path(path_ptr, path_len, out, cap);
}

int64_t fv_app_file_size(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (fv_win_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  struct stat st;
  if (stat(resolved, &st) != 0) return -1;
  if (!(st.st_mode & S_IFREG)) return -2;
  return (int64_t)st.st_size;
}

int32_t fv_app_file_same_bytes(const char *path_ptr, int32_t path_len, int64_t addr, int32_t data_len) {
  char resolved[4096];
  if (fv_win_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  const unsigned char *want = (const unsigned char *)(intptr_t)addr;
  if (data_len < 0 || (data_len > 0 && !want)) return 0;
  FILE *f = fopen(resolved, "rb");
  if (!f) return 0;
  unsigned char buf[65536];
  int32_t off = 0;
  int same = 1;
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if ((int64_t)off + (int64_t)n > data_len || memcmp(buf, want + off, n) != 0) {
      same = 0;
      break;
    }
    off += (int32_t)n;
  }
  if (ferror(f)) same = 0;
  fclose(f);
  return same && off == data_len ? 1 : 0;
}

int32_t fv_app_write_new_file_at(const char *path_ptr, int32_t path_len, int64_t addr, int32_t data_len) {
  char resolved[4096];
  if (fv_win_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  const void *data = (const void *)(intptr_t)addr;
  if (data_len < 0 || (data_len > 0 && !data)) return -1;
  fv_mkdirs_parent(resolved);
  HANDLE h = CreateFileA(resolved, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS ? 1 : -1;
  const char *p = (const char *)data;
  int32_t left = data_len;
  int bad = 0;
  while (left > 0) {
    DWORD wrote = 0;
    if (!WriteFile(h, p, (DWORD)left, &wrote, NULL) || wrote == 0) {
      bad = 1;
      break;
    }
    p += wrote;
    left -= (int32_t)wrote;
  }
  if (!CloseHandle(h)) bad = 1;
  if (bad) {
    DeleteFileA(resolved);
    return -1;
  }
  return 0;
}

#else /* POSIX */

extern char **environ;

typedef struct {
  pid_t pid;
  int killing;
} SplitPidEntry;

static pthread_mutex_t g_split_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_split_cond = PTHREAD_COND_INITIALIZER;
static SplitPidEntry *g_split_entries = NULL;
static int g_split_count = 0;
static int g_split_cap = 0;
static int g_atexit_registered = 0;
static int g_shutting_down = 0;

static int find_split_entry_locked(pid_t p) {
  for (int i = 0; i < g_split_count; i++) {
    if (g_split_entries[i].pid == p) return i;
  }
  return -1;
}

static int ensure_split_cap_locked(void) {
  if (g_split_count < g_split_cap) return 1;
  int next_cap = g_split_cap == 0 ? 16 : g_split_cap * 2;
  SplitPidEntry *next = (SplitPidEntry *)realloc(g_split_entries, (size_t)next_cap * sizeof(SplitPidEntry));
  if (!next) return 0;
  g_split_entries = next;
  g_split_cap = next_cap;
  return 1;
}

static void track_split_pid_locked(pid_t p) {
  if (p <= 0) return;
  if (find_split_entry_locked(p) >= 0) return;
  if (g_split_count < g_split_cap) {
    g_split_entries[g_split_count].pid = p;
    g_split_entries[g_split_count].killing = 0;
    g_split_count++;
  }
}

static void untrack_split_pid_locked(pid_t p) {
  int idx = find_split_entry_locked(p);
  if (idx >= 0) {
    g_split_entries[idx] = g_split_entries[--g_split_count];
    pthread_cond_broadcast(&g_split_cond);
  }
}

static pid_t waitpid_nohang_retry(pid_t p, int *st, int *err_out) {
  for (;;) {
    pid_t r = waitpid(p, st, WNOHANG);
    if (r < 0 && errno == EINTR) continue;
    if (err_out) *err_out = r < 0 ? errno : 0;
    return r;
  }
}

int32_t fv_app_kill_split_host(int32_t pid) {
  if (pid <= 0) return 0;
  pid_t p = (pid_t)pid;
  pthread_mutex_lock(&g_split_mu);
  int idx = find_split_entry_locked(p);
  if (idx < 0) {
    // Unregistered (already reaped/untracked) PID: NEVER waitpid/kill a possibly reused PID.
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }
  if (g_split_entries[idx].killing) {
    pthread_mutex_unlock(&g_split_mu);
    return 0;
  }
  g_split_entries[idx].killing = 1;
  int fast_kill = g_shutting_down;
  pthread_mutex_unlock(&g_split_mu);

  int st = 0;
  int err = 0;
  pid_t r = waitpid_nohang_retry(p, &st, &err);
  if (r == p) {
    pthread_mutex_lock(&g_split_mu);
    untrack_split_pid_locked(p);
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }
  if (r < 0 && err == ECHILD) {
    // Not our child (or already reaped): NEVER signal an unrelated/reused PID.
    pthread_mutex_lock(&g_split_mu);
    untrack_split_pid_locked(p);
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }

  if (!fast_kill) {
    kill(p, SIGTERM);
    for (int i = 0; i < 15; i++) {
      r = waitpid_nohang_retry(p, &st, &err);
      if (r == p || (r < 0 && err == ECHILD)) {
        pthread_mutex_lock(&g_split_mu);
        untrack_split_pid_locked(p);
        pthread_mutex_unlock(&g_split_mu);
        return 1;
      }
      pthread_mutex_lock(&g_split_mu);
      int sd = g_shutting_down;
      pthread_mutex_unlock(&g_split_mu);
      if (sd) break;
      usleep(10000);
    }
  }
  kill(p, SIGKILL);
  // Bounded waitpid after SIGKILL (up to 500ms); keep entry tracked and return 0 if still un-reaped.
  for (int i = 0; i < 50; i++) {
    r = waitpid_nohang_retry(p, &st, &err);
    if (r == p || (r < 0 && err == ECHILD)) {
      pthread_mutex_lock(&g_split_mu);
      untrack_split_pid_locked(p);
      pthread_mutex_unlock(&g_split_mu);
      return 1;
    }
    usleep(10000);
  }
  pthread_mutex_lock(&g_split_mu);
  int rem_idx = find_split_entry_locked(p);
  if (rem_idx >= 0) g_split_entries[rem_idx].killing = 0;
  pthread_cond_broadcast(&g_split_cond);
  pthread_mutex_unlock(&g_split_mu);
  return 0;
}

void fv_app_cleanup_split_hosts(void) {
  pthread_mutex_lock(&g_split_mu);
  g_shutting_down = 1;
  pthread_cond_broadcast(&g_split_cond);
  int n = g_split_count;
  pid_t *snapshot = n > 0 ? (pid_t *)malloc((size_t)n * sizeof(pid_t)) : NULL;
  if (snapshot) {
    for (int i = 0; i < n; i++) {
      snapshot[i] = g_split_entries[i].pid;
    }
  }
  pthread_mutex_unlock(&g_split_mu);

  if (!snapshot) return;
  for (int i = 0; i < n; i++) {
    pid_t p = snapshot[i];
    if (p <= 0) continue;
    if (fv_app_kill_split_host((int32_t)p) != 0) continue;

    // Another worker currently holds `killing == 1` (or timed out): wait via condvar up to 1200ms
    // (covers worker observing g_shutting_down, exiting SIGTERM grace, and completing 500ms SIGKILL).
    // Strict single-reaper invariant: NEVER take over if `killing` is still 1 after timeout.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    ts.tv_nsec += 200000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_split_mu);
    int idx = find_split_entry_locked(p);
    while (idx >= 0 && g_split_entries[idx].killing) {
      int wr = pthread_cond_timedwait(&g_split_cond, &g_split_mu, &ts);
      idx = find_split_entry_locked(p);
      if (wr == ETIMEDOUT) break;
    }
    int take_over = (idx >= 0 && !g_split_entries[idx].killing);
    if (take_over) g_split_entries[idx].killing = 1;
    pthread_mutex_unlock(&g_split_mu);

    if (take_over) {
      int st = 0;
      int err = 0;
      pid_t r = waitpid_nohang_retry(p, &st, &err);
      int reaped = (r == p || (r < 0 && err == ECHILD));
      if (!reaped) {
        kill(p, SIGKILL);
        for (int k = 0; k < 50; k++) {
          r = waitpid_nohang_retry(p, &st, &err);
          if (r == p || (r < 0 && err == ECHILD)) {
            reaped = 1;
            break;
          }
          usleep(10000);
        }
      }
      pthread_mutex_lock(&g_split_mu);
      if (reaped) {
        untrack_split_pid_locked(p);
      } else {
        int ridx = find_split_entry_locked(p);
        if (ridx >= 0) g_split_entries[ridx].killing = 0;
        pthread_cond_broadcast(&g_split_cond);
      }
      pthread_mutex_unlock(&g_split_mu);
    }
  }
  free(snapshot);
}

int32_t fv_app_reap_pid(int32_t pid) {
  if (pid <= 0) return 1;
  pid_t p = (pid_t)pid;
  pthread_mutex_lock(&g_split_mu);
  int idx = find_split_entry_locked(p);
  if (idx < 0) {
    // Unregistered (already reaped/untracked) PID: NEVER waitpid a possibly reused PID.
    pthread_mutex_unlock(&g_split_mu);
    return 1;
  }
  if (g_split_entries[idx].killing) {
    pthread_mutex_unlock(&g_split_mu);
    return 0;
  }
  g_split_entries[idx].killing = 1;
  pthread_mutex_unlock(&g_split_mu);

  int st = 0;
  int err = 0;
  pid_t r = waitpid_nohang_retry(p, &st, &err);
  int reaped = (r == p || (r < 0 && err == ECHILD));
  pthread_mutex_lock(&g_split_mu);
  if (reaped) {
    untrack_split_pid_locked(p);
  } else {
    int ridx = find_split_entry_locked(p);
    if (ridx >= 0) g_split_entries[ridx].killing = 0;
    pthread_cond_broadcast(&g_split_cond);
  }
  pthread_mutex_unlock(&g_split_mu);
  return reaped ? 1 : 0;
}

int32_t fv_app_pick_free_port(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(0);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
    close(s);
    return -1;
  }
  socklen_t n = sizeof a;
  int port = -1;
  if (getsockname(s, (struct sockaddr *)&a, &n) == 0) {
    port = (int)ntohs(a.sin_port);
  }
  close(s);
  return (int32_t)port;
}

static int resolve_host_binary(char *out, size_t out_sz) {
  const char *env_bin = getenv("FV_HOST_BIN");
  if (env_bin && env_bin[0] != '\0' && access(env_bin, X_OK) == 0) {
    snprintf(out, out_sz, "%s", env_bin);
    return 1;
  }
  char self_path[PATH_MAX];
  int have_self = 0;
#ifdef __APPLE__
  uint32_t sz = (uint32_t)sizeof(self_path);
  if (_NSGetExecutablePath(self_path, &sz) == 0) have_self = 1;
#else
  ssize_t nr = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
  if (nr > 0 && nr < (ssize_t)sizeof(self_path)) {
    self_path[nr] = '\0';
    have_self = 1;
  }
#endif
  if (have_self) {
    char *slash = strrchr(self_path, '/');
    if (slash) {
      *slash = '\0';
      snprintf(out, out_sz, "%s/pion-host", self_path);
      if (access(out, X_OK) == 0) return 1;
    }
  }
  if (access("./out/pion-host", X_OK) == 0) {
    snprintf(out, out_sz, "%s", "./out/pion-host");
    return 1;
  }
  snprintf(out, out_sz, "%s", "pion-host");
  return 0;
}

int32_t fv_app_spawn_split_host(int32_t port, const void *url_ptr, int32_t url_len) {
  if (port <= 0) return -1;
  pthread_mutex_lock(&g_split_mu);
  if (g_shutting_down) {
    pthread_mutex_unlock(&g_split_mu);
    return -1;
  }
  if (!g_atexit_registered) {
    atexit(fv_app_cleanup_split_hosts);
    g_atexit_registered = 1;
  }
  pthread_mutex_unlock(&g_split_mu);

  char host_bin[PATH_MAX];
  int is_path = resolve_host_binary(host_bin, sizeof(host_bin));

  char port_s[32];
  snprintf(port_s, sizeof(port_s), "%d", (int)port);

  const char *fallback_url = "about:blank";
  char *url_buf = NULL;
  if (url_ptr != NULL && url_len > 0) {
    url_buf = (char *)malloc((size_t)url_len + 1);
    if (!url_buf) return -1;
    memcpy(url_buf, url_ptr, (size_t)url_len);
    url_buf[url_len] = '\0';
  }
  char *argv[] = {
    host_bin,
    port_s,
    url_buf ? url_buf : (char *)fallback_url,
    NULL
  };

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

  // The secondary split-pane host never writes the primary browser's files: FV_PROFILE_DIR (its workspace,
  // passkeys) and FV_STORAGE are stripped, and FV_STATE_DIR points at a state directory of its own
  // (<state>/split-<port>) seeded with a copy of the primary's cookies and localStorage, so the sites signed in
  // in the main pane stay signed in in Split View, while its url, host.json and what it saves stay its own (never
  // the primary's, never the app's working directory).
  char split_state[PATH_MAX];
  split_state[0] = '\0';
  char split_state_env[PATH_MAX + 16];
  split_state_env[0] = '\0';
  if (fv_split_state_dir((int)port, split_state, sizeof(split_state)) == 0) {
    snprintf(split_state_env, sizeof(split_state_env), "FV_STATE_DIR=%s", split_state);
  }
  int env_count = 0;
  if (environ) {
    while (environ[env_count] != NULL) env_count++;
  }
  char **child_env = (char **)malloc((size_t)(env_count + 2) * sizeof(char *));
  int ce = 0;
  if (child_env && environ) {
    for (int i = 0; i < env_count; i++) {
      if (fv_app_split_env_dropped(environ[i])) continue;
      child_env[ce++] = environ[i];
    }
  }
  if (child_env) {
    if (split_state_env[0] != '\0') child_env[ce++] = split_state_env;
    child_env[ce] = NULL;
  }

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  pid_t pid = -1;
  char **spawn_env = child_env ? child_env : environ;

  // Hold g_split_mu across shutting_down check + capacity reservation + posix_spawn + tracking
  // so atexit/cleanup can never miss a freshly spawned child and registration cannot OOM after spawn.
  pthread_mutex_lock(&g_split_mu);
  if (g_shutting_down || !ensure_split_cap_locked()) {
    pthread_mutex_unlock(&g_split_mu);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    if (child_env) free(child_env);
    if (url_buf) free(url_buf);
    return -1;
  }
  int rc = is_path
    ? posix_spawn(&pid, host_bin, &fa, &attr, argv, spawn_env)
    : posix_spawnp(&pid, host_bin, &fa, &attr, argv, spawn_env);
  if (rc == 0 && pid > 0) {
    track_split_pid_locked(pid);
  }
  pthread_mutex_unlock(&g_split_mu);

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (child_env) free(child_env);
  if (url_buf) free(url_buf);

  if (rc != 0 || pid <= 0) return -1;
  return (int32_t)pid;
}

int32_t fv_app_verify_local_file(const char *path_ptr, int32_t path_len, int64_t expected_bytes) {
  char resolved[4096];
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  struct stat st;
  if (stat(resolved, &st) != 0) return 0;
  if (!S_ISREG(st.st_mode)) return 0;
  if (expected_bytes > 0 && (int64_t)st.st_size != expected_bytes) return 0;
  return 1;
}

static void *fv_reveal_reaper_thread(void *arg) {
  pid_t pid = (pid_t)(intptr_t)arg;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return NULL;
}

int32_t fv_app_reveal_file(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  // (absolute only: the opener never reads it as an option)
  if (!fv_path_is_absolute(resolved) || access(resolved, F_OK) != 0) return -1;

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  pid_t pid = -1;
  int rc = -1;
#ifdef __APPLE__
  char *argv[] = { (char *)"/usr/bin/open", (char *)"-R", resolved, NULL };
  rc = posix_spawn(&pid, "/usr/bin/open", &fa, &attr, argv, environ);
#else
  char dir_buf[4096];
  memcpy(dir_buf, resolved, strlen(resolved) + 1);
  char *slash = strrchr(dir_buf, '/');
  if (slash) {
    if (slash == dir_buf) {
      slash[1] = '\0';
    } else {
      *slash = '\0';
    }
  } else {
    dir_buf[0] = '.';
    dir_buf[1] = '\0';
  }
  char *argv[] = { (char *)"xdg-open", dir_buf, NULL };
  rc = posix_spawnp(&pid, "xdg-open", &fa, &attr, argv, environ);
#endif

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (rc == 0 && pid > 0) {
    pthread_t th;
    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &tattr, fv_reveal_reaper_thread, (void *)(intptr_t)pid) != 0) {
      int st = 0;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    pthread_attr_destroy(&tattr);
    return 0;
  }
  return -1;
}

/* Opens a local file with the system's default app (`open` / `xdg-open`); 0 when the opener was started. Only an
 * absolute path to a regular file (no directory, no symlink) of the document and media types above
 * (`fv_app_open_target_ok`); on macOS the file is marked as downloaded from the internet first (when it is not
 * already), through the file opened without following a link, so Gatekeeper and the opening app treat it as such. */
int32_t fv_app_open_file(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (!fv_app_open_target_ok(path_ptr, path_len)) return -1;
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
#ifdef __APPLE__
  {
    int qfd = open(resolved, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (qfd < 0) return -1;
    struct stat qst;
    if (fstat(qfd, &qst) != 0 || !S_ISREG(qst.st_mode)) {
      close(qfd);
      return -1;
    }
    if (fgetxattr(qfd, "com.apple.quarantine", NULL, 0, 0, 0) < 0) {
      char q[64];
      int qn = snprintf(q, sizeof(q), "0081;%08lx;Farview;", (unsigned long)time(NULL));
      if (qn > 0) (void)fsetxattr(qfd, "com.apple.quarantine", q, (size_t)qn, 0, 0);
    }
    close(qfd);
  }
#endif

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  pid_t pid = -1;
  int rc = -1;
#ifdef __APPLE__
  char *argv[] = { (char *)"/usr/bin/open", resolved, NULL };
  rc = posix_spawn(&pid, "/usr/bin/open", &fa, &attr, argv, environ);
#else
  char *argv[] = { (char *)"xdg-open", resolved, NULL };
  rc = posix_spawnp(&pid, "xdg-open", &fa, &attr, argv, environ);
#endif

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (rc == 0 && pid > 0) {
    pthread_t th;
    pthread_attr_t tattr;
    pthread_attr_init(&tattr);
    pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &tattr, fv_reveal_reaper_thread, (void *)(intptr_t)pid) != 0) {
      int st = 0;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    pthread_attr_destroy(&tattr);
    return 0;
  }
  return -1;
}

/* A download's file on this machine (client_downloads.pion), all called on the downloads worker thread, never on
 * the render thread. A path holding a NUL is refused (the name Pion keeps must be the one on disk). */

/* The address of a byte buffer the caller keeps alive while the worker reads it. */
int64_t fv_app_bytes_addr(const void *p) { return (int64_t)(intptr_t)p; }

static int fv_dl_resolve(const char *path_ptr, int32_t path_len, char *out, size_t cap) {
  if (!path_ptr || path_len <= 0) return -1;
  if (memchr(path_ptr, 0, (size_t)path_len)) return -1;
  return fv_resolve_home_path(path_ptr, path_len, out, cap);
}

/* The size of the regular file at `path`, -1 when there is none. */
int64_t fv_app_file_size(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (fv_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  struct stat st;
  if (stat(resolved, &st) != 0) return -1;
  if (!S_ISREG(st.st_mode)) return -2;
  return (int64_t)st.st_size;
}

/* 1 when the file at `path` holds exactly `data_len` bytes at `addr`, else 0. */
int32_t fv_app_file_same_bytes(const char *path_ptr, int32_t path_len, int64_t addr, int32_t data_len) {
  char resolved[4096];
  if (fv_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  const unsigned char *want = (const unsigned char *)(intptr_t)addr;
  if (data_len < 0 || (data_len > 0 && !want)) return 0;
  int fd = open(resolved, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  unsigned char buf[65536];
  int32_t off = 0;
  int same = 1;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      same = 0;
      break;
    }
    if (n == 0) break;
    if ((int64_t)off + n > data_len || memcmp(buf, want + off, (size_t)n) != 0) {
      same = 0;
      break;
    }
    off += (int32_t)n;
  }
  close(fd);
  return same && off == data_len ? 1 : 0;
}

/* Writes a file the host downloaded into this machine's own folder: `path` (may start with ~/), always a new file
 * (never over one already there), its folders made when missing (mkdir -p); on macOS marked as downloaded from
 * the internet.
 * 0 written, 1 a file of that name is already there, -1 failed. */
int32_t fv_app_write_new_file_at(const char *path_ptr, int32_t path_len, int64_t addr, int32_t data_len) {
  char resolved[4096];
  if (fv_dl_resolve(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return -1;
  const void *data = (const void *)(intptr_t)addr;
  if (data_len < 0 || (data_len > 0 && !data)) return -1;
  fv_mkdirs_parent(resolved);
  int fd = open(resolved, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return errno == EEXIST ? 1 : -1;
  const char *p = (const char *)data;
  int32_t left = data_len;
  while (left > 0) {
    ssize_t n = write(fd, p, (size_t)left);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlink(resolved);
      return -1;
    }
    p += n;
    left -= (int32_t)n;
  }
#ifdef __APPLE__
  // (marked through the descriptor written, never the path again)
  char q[64];
  int qn = snprintf(q, sizeof(q), "0081;%08lx;Farview;", (unsigned long)time(NULL));
  if (qn > 0) (void)fsetxattr(fd, "com.apple.quarantine", q, (size_t)qn, 0, 0);
#endif
  if (close(fd) != 0) {
    unlink(resolved);
    return -1;
  }
  return 0;
}
#endif /* _WIN32 */
