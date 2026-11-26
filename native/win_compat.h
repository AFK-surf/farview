#ifndef FV_WIN_COMPAT_H
#define FV_WIN_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef X_OK
#define X_OK 0
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef getpid
#define getpid _getpid
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef rmdir
#define rmdir _rmdir
#endif
#ifndef access
#define access _access
#endif
#ifndef lstat
#define lstat stat
#endif
#ifndef fsync
#define fsync _commit
#endif
#ifndef fileno
#define fileno _fileno
#endif
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif
#ifndef strdup
#define strdup _strdup
#endif

static inline int fv_win_ftruncate(int fd, int64_t len) {
  return _chsize_s(fd, len) == 0 ? 0 : -1;
}
#define ftruncate(fd, len) fv_win_ftruncate((fd), (int64_t)(len))

static inline char *fv_win_strndup(const char *s, size_t n) {
  if (!s) return NULL;
  size_t len = strnlen(s, n);
  char *out = (char *)malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}
#define strndup(s, n) fv_win_strndup((s), (n))

typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
static BOOL CALLBACK fv_win_once_tramp(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)ctx;
  void (*fn)(void) = (void (*)(void))param;
  if (fn) fn();
  return TRUE;
}
static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
  InitOnceExecuteOnce(once, fv_win_once_tramp, (PVOID)fn, NULL);
  return 0;
}

static inline int fv_win_mkdir_compat(const char *path, int mode) {
  (void)mode;
  return _mkdir(path);
}
#define mkdir(p, m) fv_win_mkdir_compat((p), (m))

static inline int fv_win_setenv_compat(const char *k, const char *v, int overwrite) {
  if (!k || !v) return -1;
  if (!overwrite && getenv(k) != NULL) return 0;
  return _putenv_s(k, v) == 0 ? 0 : -1;
}
static inline int fv_win_unsetenv_compat(const char *k) {
  if (!k) return -1;
  return _putenv_s(k, "") == 0 ? 0 : -1;
}
#define setenv(k, v, ow) fv_win_setenv_compat((k), (v), (ow))
#define unsetenv(k) fv_win_unsetenv_compat((k))

static inline void fv_win_usleep(uint64_t us) {
  if (us == 0) return;
  DWORD ms = (DWORD)((us + 999ULL) / 1000ULL);
  if (ms == 0) ms = 1;
  Sleep(ms);
}
#ifndef usleep
#define usleep(us) fv_win_usleep((uint64_t)(us))
#endif

/* One-time Winsock2 initialization */
static BOOL CALLBACK fv_win_wsa_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  return TRUE;
}
static inline void fv_win_wsa_init(void) {
  static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&g_wsa_once, fv_win_wsa_init_cb, NULL, NULL);
}

/* Clock helpers */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int fv_win_clock_gettime(int clk_id, struct timespec *ts) {
  if (!ts) return -1;
  if (clk_id == CLOCK_MONOTONIC) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    ts->tv_sec = (time_t)(cnt.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)(((cnt.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    return 0;
  }
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t100 = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  const uint64_t epoch_diff = 116444736000000000ULL;
  uint64_t unix100 = t100 > epoch_diff ? (t100 - epoch_diff) : 0ULL;
  ts->tv_sec = (time_t)(unix100 / 10000000ULL);
  ts->tv_nsec = (long)((unix100 % 10000000ULL) * 100ULL);
  return 0;
}
#define clock_gettime(id, ts) fv_win_clock_gettime((id), (ts))

/* dlopen / dlsym / dlclose shim */
#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

static inline void *fv_win_dlopen(const char *name, int flags) {
  (void)flags;
  if (!name) return (void *)GetModuleHandleA(NULL);
  return (void *)LoadLibraryA(name);
}
static inline void *fv_win_dlsym(void *handle, const char *sym) {
  if (!handle || !sym) return NULL;
  return (void *)GetProcAddress((HMODULE)handle, sym);
}
static inline int fv_win_dlclose(void *handle) {
  if (!handle) return 0;
  return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
#define dlopen(n, f) fv_win_dlopen((n), (f))
#define dlsym(h, s) fv_win_dlsym((h), (s))
#define dlclose(h) fv_win_dlclose((h))

/* Minimal pthread shim backed by SRWLOCK + CONDITION_VARIABLE + _beginthreadex */
#ifndef FV_WIN_PTHREAD_SHIM_DEFINED
#define FV_WIN_PTHREAD_SHIM_DEFINED

typedef SRWLOCK pthread_mutex_t;
typedef int pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
  (void)attr;
  InitializeSRWLock(m);
  return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
  (void)m;
  return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
  AcquireSRWLockExclusive(m);
  return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
  ReleaseSRWLockExclusive(m);
  return 0;
}

typedef CONDITION_VARIABLE pthread_cond_t;
typedef int pthread_condattr_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
  (void)attr;
  InitializeConditionVariable(c);
  return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c) {
  (void)c;
  return 0;
}
static inline int pthread_cond_signal(pthread_cond_t *c) {
  WakeConditionVariable(c);
  return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
  WakeAllConditionVariable(c);
  return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
  struct timespec now;
  fv_win_clock_gettime(CLOCK_REALTIME, &now);
  int64_t diff_ms = ((int64_t)abstime->tv_sec - (int64_t)now.tv_sec) * 1000LL +
                    ((int64_t)abstime->tv_nsec - (int64_t)now.tv_nsec) / 1000000LL;
  DWORD wait_ms = diff_ms <= 0 ? 0 : (DWORD)diff_ms;
  if (SleepConditionVariableSRW(c, m, wait_ms, 0)) return 0;
  return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : -1;
}

typedef HANDLE pthread_t;
typedef struct {
  int detached;
} pthread_attr_t;
#define PTHREAD_CREATE_DETACHED 1

static inline int pthread_attr_init(pthread_attr_t *a) {
  if (a) a->detached = 0;
  return 0;
}
static inline int pthread_attr_destroy(pthread_attr_t *a) {
  (void)a;
  return 0;
}
static inline int pthread_attr_setdetachstate(pthread_attr_t *a, int s) {
  if (a) a->detached = (s == PTHREAD_CREATE_DETACHED);
  return 0;
}

typedef struct {
  void *(*fn)(void *);
  void *arg;
} fv_win_thread_tramp_t;

static unsigned __stdcall fv_win_thread_entry(void *p) {
  fv_win_thread_tramp_t *t = (fv_win_thread_tramp_t *)p;
  void *(*fn)(void *) = t->fn;
  void *arg = t->arg;
  free(t);
  (void)fn(arg);
  return 0;
}

static inline int pthread_create(pthread_t *tid, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  fv_win_thread_tramp_t *t = (fv_win_thread_tramp_t *)malloc(sizeof(*t));
  if (!t) return ENOMEM;
  t->fn = fn;
  t->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, fv_win_thread_entry, t, 0, NULL);
  if (h == 0) {
    free(t);
    return EAGAIN;
  }
  if (attr && attr->detached) {
    CloseHandle((HANDLE)h);
    if (tid) *tid = NULL;
  } else {
    if (tid) *tid = (HANDLE)h;
    else CloseHandle((HANDLE)h);
  }
  return 0;
}
static inline int pthread_detach(pthread_t tid) {
  if (tid) CloseHandle(tid);
  return 0;
}
static inline int pthread_join(pthread_t tid, void **retval) {
  if (retval) *retval = NULL;
  if (!tid) return 0;
  WaitForSingleObject(tid, INFINITE);
  CloseHandle(tid);
  return 0;
}

#endif /* FV_WIN_PTHREAD_SHIM_DEFINED */

/* Socket helpers (Winsock SOCKET != CRT fd) */
#define fv_sock_t SOCKET
#define FV_SOCK_INVALID INVALID_SOCKET
#define fv_sock_from_i32(fd) ((SOCKET)(uintptr_t)(uint32_t)(fd))
#define fv_sock_to_i32(s) ((s) == INVALID_SOCKET ? -1 : (int32_t)(uint32_t)(uintptr_t)(s))
#define fv_sock_close(fd) closesocket(fv_sock_from_i32(fd))
#define fv_sock_recv(fd, buf, len, fl) recv(fv_sock_from_i32(fd), (char *)(buf), (int)(len), (fl))
#define fv_sock_send(fd, buf, len, fl) send(fv_sock_from_i32(fd), (const char *)(buf), (int)(len), (fl))
#define fv_sock_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#define fv_sock_eintr() (WSAGetLastError() == WSAEINTR)
#define fv_sock_inprogress() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS)
static inline int fv_sock_set_nb(int32_t fd, int nb) {
  u_long mode = nb ? 1UL : 0UL;
  return ioctlsocket(fv_sock_from_i32(fd), FIONBIO, &mode) == 0 ? 0 : -1;
}
static inline int fv_sock_poll(WSAPOLLFD *fds, ULONG nfds, int timeout_ms) {
  if (nfds == 0) {
    if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
    return 0;
  }
  return WSAPoll(fds, nfds, timeout_ms);
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

#define fv_win_wsa_init() ((void)0)
#define fv_sock_t int
#define FV_SOCK_INVALID (-1)
#define fv_sock_from_i32(fd) (fd)
#define fv_sock_to_i32(s) ((int32_t)(s))
#define fv_sock_close(fd) close(fd)
#define fv_sock_recv(fd, buf, len, fl) recv((fd), (buf), (len), (fl))
#define fv_sock_send(fd, buf, len, fl) send((fd), (buf), (len), (fl))
#define fv_sock_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#define fv_sock_eintr() (errno == EINTR)
#define fv_sock_inprogress() (errno == EINPROGRESS)
static inline int fv_sock_set_nb(int32_t fd, int nb) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  if (nb) fl |= O_NONBLOCK;
  else fl &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, fl);
}
static inline int fv_sock_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
  return poll(fds, nfds, timeout_ms);
}

#endif /* _WIN32 */

#endif /* FV_WIN_COMPAT_H */
