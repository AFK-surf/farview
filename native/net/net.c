// TCP sockets for host and client: non-blocking reads, blocking writes.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <moonbit.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int32_t fv_listen(int32_t port) {
  // a client gone mid-write is a failed send, not the end of the host
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (bind(s, (struct sockaddr*)&a, sizeof a) < 0 || listen(s, 8) < 0) return -1;
  return s;
}

// Listening on one address only (`addr`, e.g. 127.0.0.1); port 0 takes a free one (`fv_port_of` says which).
int32_t fv_listen_on(moonbit_bytes_t addr, int32_t port) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, (const char*)addr, &a.sin_addr) != 1) {
    close(s);
    return -1;
  }
  if (bind(s, (struct sockaddr*)&a, sizeof a) < 0 || listen(s, 8) < 0) {
    close(s);
    return -1;
  }
  return s;
}

// The port a socket is bound to.
int32_t fv_port_of(int32_t s) {
  struct sockaddr_in a = {0};
  socklen_t n = sizeof a;
  if (getsockname(s, (struct sockaddr*)&a, &n) < 0) return -1;
  return ntohs(a.sin_port);
}

// Waits up to wait_ms for any of the n sockets to have something to read (or a connection to take); the number that do.
int32_t fv_poll(int32_t* fds, int32_t n, int32_t wait_ms) {
  struct pollfd p[64];
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) {
    p[i].fd = fds[i];
    p[i].events = POLLIN;
    p[i].revents = 0;
  }
  int r = poll(p, n, wait_ms);
  return r < 0 ? 0 : r;
}

static volatile sig_atomic_t term_asked = 0;
static void on_term(int sig) { (void)sig; term_asked = 1; }

// SIGTERM and SIGINT noted rather than ending the process at once (`fv_term_asked`): what it keeps is written first.
void fv_catch_term(void) {
  struct sigaction sa = {0};
  sa.sa_handler = on_term;
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGINT, &sa, 0);
}

int32_t fv_term_asked(void) { return term_asked; }

int32_t fv_getpid(void) { return getpid(); }

#include <stdlib.h>
void fv_setenv(moonbit_bytes_t k, moonbit_bytes_t v) { setenv((const char*)k, (const char*)v, 1); }

int32_t fv_accept(int32_t s) {
  int c = accept(s, 0, 0);
  int on = 1;
  if (c >= 0) setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  return c;
}

// Whether the process that started this one has ended (its children are then the system's: a page process outliving
// its host)
int32_t fv_parent_gone(void) { return getppid() == 1; }

// A connection waiting on a listening socket, or -1 at once when none is.
int32_t fv_accept_now(int32_t s) {
  struct pollfd p = {s, POLLIN, 0};
  if (poll(&p, 1, 0) <= 0) return -1;
  return fv_accept(s);
}

int32_t fv_connect(moonbit_bytes_t host, int32_t port) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, (const char*)host, &a.sin_addr) != 1) return -1;
  if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) return -1;
  int on = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  return s;
}

int32_t fv_send(int32_t s, moonbit_bytes_t b, int32_t n) {
  int32_t off = 0;
  while (off < n) {
    ssize_t k = write(s, b + off, n - off);
    if (k <= 0) return -1;
    off += k;
  }
  return n;
}

// Up to n bytes; 0 when nothing is there within wait_ms; -1 when closed.
int32_t fv_recv(int32_t s, moonbit_bytes_t b, int32_t n, int32_t wait_ms) {
  struct pollfd p = {s, POLLIN, 0};
  int r = poll(&p, 1, wait_ms);
  if (r <= 0) return 0;
  ssize_t k = read(s, b, n);
  return k <= 0 ? -1 : (int32_t)k;
}

#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// A child process for one connection (0 in the child, its pid in the parent, -1 when none could be made).
int32_t fv_fork(void) { return fork(); }

// Spawns a fresh copy of the current executable via fork()+execv() to serve an accepted client socket.
// On macOS, bare fork() without exec() invalidates Mach/XPC/libdispatch state in the child, causing
// Apple OpenGL-on-Metal (GLMetal) MTLCompilerService XPC calls to fail buildPipelineState
// (m_disable_code: 1001000) on uncached shaders. Re-execing gives the child a clean OS/Metal/XPC state
// while preserving full per-connection crash isolation.
int32_t fv_spawn_self_serve(int32_t sock_fd, int32_t close_fd, int32_t port, int32_t agent_port,
                            moonbit_bytes_t url, int32_t url_len) {
  char exe[PATH_MAX];
#ifdef __APPLE__
  uint32_t sz = (uint32_t)sizeof(exe);
  if (_NSGetExecutablePath(exe, &sz) != 0) return -1;
#else
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0 || n >= (ssize_t)sizeof(exe)) return -1;
  exe[n] = '\0';
#endif
  if (url_len < 0) url_len = 0;
  char* url_buf = (char*)malloc((size_t)url_len + 1);
  if (!url_buf) return -1;
  if (url_len > 0) memcpy(url_buf, url, (size_t)url_len);
  url_buf[url_len] = '\0';

  char fd_s[32], port_s[32], agent_s[32];
  snprintf(fd_s, sizeof(fd_s), "%d", (int)sock_fd);
  snprintf(port_s, sizeof(port_s), "%d", (int)port);
  snprintf(agent_s, sizeof(agent_s), "%d", (int)agent_port);

  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGPIPE, SIG_IGN);
    if (close_fd >= 0 && close_fd != sock_fd) close(close_fd);
    int fl = fcntl(sock_fd, F_GETFD);
    if (fl >= 0 && (fl & FD_CLOEXEC)) fcntl(sock_fd, F_SETFD, fl & ~FD_CLOEXEC);
    char* argv[] = {exe, (char*)"--serve-fd", fd_s, agent_s, port_s, url_buf, NULL};
    execv(exe, argv);
    _exit(127);
  }
  free(url_buf);
  return (int32_t)pid;
}

// Waits for a child; 0 when it ended normally, else its signal or exit status (a page that ended the engine).
int32_t fv_wait(int32_t pid) {
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return WEXITSTATUS(st);
}

void fv_close(int32_t s) { close(s); }

// (the sampler's file, FV_SAMPLE, written first: a page process ends here, past atexit; fetch.c's sampler replaces
// this empty one in the programs that have it)
__attribute__((weak)) void fv_sample_flush(void) {}
void fv_exit(int32_t code) {
  fv_sample_flush();
  _exit(code);
}

#include <stdio.h>
#include <string.h>
// Adds text at the end of a file (a log several processes write).
void fv_append(moonbit_bytes_t path, moonbit_bytes_t text, int32_t n) {
  FILE* f = fopen((const char*)path, "a");
  if (!f) return;
  fwrite(text, 1, n, f);
  fclose(f);
}

// Writes a file's whole text (what was there before is gone).
void fv_write_file(moonbit_bytes_t path, moonbit_bytes_t text, int32_t n) {
  FILE* f = fopen((const char*)path, "w");
  if (!f) return;
  fwrite(text, 1, n, f);
  fclose(f);
}

// A file's whole content written beside it, then moved over it: one who reads it (or a crash meanwhile) sees the old
// content or the new, never part of it.
void fv_write_file_atomic(moonbit_bytes_t path, moonbit_bytes_t data, int32_t n) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s.tmp%d", (const char*)path, (int)getpid());
  FILE* f = fopen(tmp, "wb");
  if (!f) return;
  fwrite(data, 1, n, f);
  if (fclose(f) != 0) {
    unlink(tmp);
    return;
  }
  rename(tmp, (const char*)path);
}

#include <sys/stat.h>
void fv_mkdir(moonbit_bytes_t path) { mkdir((const char*)path, 0700); }

// --- Dedicated non-blocking socket, pipe, and multi-fd poll APIs ---
// Kept separate from blocking fv_send/fv_recv so host/agent/docshot/probe remain unchanged.
// Return code convention for fv_send_nb / fv_recv_nb:
//   > 0 : exact bytes transferred
//     0 : EOF (fv_recv_nb) or zero-length request (fv_send_nb)
//    -1 : real OS error
//    -2 : EAGAIN / EWOULDBLOCK
//    -3 : EINTR

#define FV_NB_ERR (-1)
#define FV_NB_WOULD_BLOCK (-2)
#define FV_NB_INTR (-3)

#define FV_POLL_IN 1
#define FV_POLL_OUT 2
#define FV_POLL_ERR 4
#define FV_POLL_HUP 8
#define FV_POLL_NVAL 16

int32_t fv_set_nonblock(int32_t fd, int32_t nonblock) {
  if (fd < 0) return -1;
  signal(SIGPIPE, SIG_IGN);
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  int nfl = nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
  if (fcntl(fd, F_SETFL, nfl) < 0) return -1;
#ifdef SO_NOSIGPIPE
  int on = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
  return 0;
}

int32_t fv_set_sndbuf(int32_t fd, int32_t size) {
  if (fd < 0 || size <= 0) return -1;
  int v = (int)size;
  return setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v) == 0 ? 0 : -1;
}

int32_t fv_set_rcvbuf(int32_t fd, int32_t size) {
  if (fd < 0 || size <= 0) return -1;
  int v = (int)size;
  return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0 ? 0 : -1;
}

int32_t fv_send_nb(int32_t fd, moonbit_bytes_t b, int32_t off, int32_t len) {
  if (fd < 0 || off < 0 || len < 0) return FV_NB_ERR;
  if (len == 0) return 0;
#ifdef MSG_NOSIGNAL
  ssize_t k = send(fd, b + off, (size_t)len, MSG_NOSIGNAL);
  if (k < 0 && errno == ENOTSOCK) {
    k = write(fd, b + off, (size_t)len);
  }
#else
  ssize_t k = write(fd, b + off, (size_t)len);
#endif
  if (k > 0) return (int32_t)k;
  if (k == 0) return FV_NB_WOULD_BLOCK;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return FV_NB_WOULD_BLOCK;
  if (errno == EINTR) return FV_NB_INTR;
  return FV_NB_ERR;
}

int32_t fv_recv_nb(int32_t fd, moonbit_bytes_t b, int32_t off, int32_t cap) {
  if (fd < 0 || off < 0 || cap <= 0) return FV_NB_ERR;
  ssize_t k = read(fd, b + off, (size_t)cap);
  if (k > 0) return (int32_t)k;
  if (k == 0) return 0;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return FV_NB_WOULD_BLOCK;
  if (errno == EINTR) return FV_NB_INTR;
  return FV_NB_ERR;
}

int32_t fv_pipe_open(int32_t* out_fds) {
  int fds[2];
  if (pipe(fds) < 0) return -1;
  for (int i = 0; i < 2; i++) {
    int fl = fcntl(fds[i], F_GETFL, 0);
    if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) < 0) {
      close(fds[0]);
      close(fds[1]);
      return -1;
    }
    int fdfl = fcntl(fds[i], F_GETFD, 0);
    if (fdfl >= 0) (void)fcntl(fds[i], F_SETFD, fdfl | FD_CLOEXEC);
  }
  out_fds[0] = (int32_t)fds[0];
  out_fds[1] = (int32_t)fds[1];
  return 0;
}

int32_t fv_pipe_wake(int32_t write_fd) {
  if (write_fd < 0) return -1;
  uint8_t byte = 1;
  ssize_t k = write(write_fd, &byte, 1);
  if (k == 1) return 0;
  if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return -1;
}

int32_t fv_pipe_drain(int32_t read_fd) {
  if (read_fd < 0) return -1;
  uint8_t tmp[64];
  int32_t total = 0;
  for (;;) {
    ssize_t k = read(read_fd, tmp, sizeof tmp);
    if (k > 0) {
      total += (int32_t)k;
      continue;
    }
    if (k == 0) {
      return total > 0 ? total : -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (errno == EINTR) continue;
    return -1;
  }
  return total;
}

int32_t fv_poll_fds(int32_t* fds, int32_t* events, int32_t* revents, int32_t n, int32_t wait_ms) {
  struct pollfd p[64];
  if (n < 0) return -1;
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) {
    p[i].fd = fds[i];
    short ev = 0;
    if (events[i] & FV_POLL_IN) ev |= POLLIN;
    if (events[i] & FV_POLL_OUT) ev |= POLLOUT;
    p[i].events = ev;
    p[i].revents = 0;
    revents[i] = 0;
  }
  int r = poll(p, (nfds_t)n, wait_ms);
  if (r < 0) {
    if (errno == EINTR) return FV_NB_INTR;
    return FV_NB_ERR;
  }
  for (int i = 0; i < n; i++) {
    int32_t rev = 0;
    if (p[i].revents & POLLIN) rev |= FV_POLL_IN;
    if (p[i].revents & POLLOUT) rev |= FV_POLL_OUT;
    if (p[i].revents & POLLERR) rev |= FV_POLL_ERR;
    if (p[i].revents & POLLHUP) rev |= FV_POLL_HUP;
    if (p[i].revents & POLLNVAL) rev |= FV_POLL_NVAL;
    revents[i] = rev;
  }
  return (int32_t)r;
}

static int64_t fv_mono_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

int32_t fv_connect_timeout(moonbit_bytes_t host, int32_t port, int32_t timeout_ms) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int on = 1;
  (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
  (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
    close(s);
    return -1;
  }
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, (const char*)host, &a.sin_addr) != 1) {
    close(s);
    return -1;
  }
  int rc = connect(s, (struct sockaddr*)&a, sizeof a);
  if (rc == 0) return s;
  if (errno != EINPROGRESS) {
    close(s);
    return -1;
  }
  int64_t deadline = timeout_ms >= 0 ? (fv_mono_ms() + (int64_t)timeout_ms) : -1;
  int pr = 0;
  for (;;) {
    int wait_ms = -1;
    if (deadline >= 0) {
      int64_t rem = deadline - fv_mono_ms();
      if (rem <= 0) {
        pr = 0;
        break;
      }
      wait_ms = rem > 2147483647 ? 2147483647 : (int)rem;
    }
    struct pollfd p = {s, POLLOUT, 0};
    pr = poll(&p, 1, wait_ms);
    if (pr < 0 && errno == EINTR) continue;
    break;
  }
  if (pr <= 0) {
    close(s);
    return -1;
  }
  int err = 0;
  socklen_t elen = sizeof err;
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
    close(s);
    return -1;
  }
  return s;
}

void fv_sleep_ms(int32_t ms) {
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}
