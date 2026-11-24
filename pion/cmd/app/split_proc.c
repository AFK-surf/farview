// Process-isolated secondary `pion-host` lifecycle manager for Arc Split View.
// Uses posix_spawn (safe in multi-threaded Metal/AppKit processes) + exact-PID
// SIGTERM/SIGKILL + waitpid reaping to guarantee zero zombie processes.

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
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

typedef struct {
  pid_t pid;
  int killing;
} SplitPidEntry;

static pthread_mutex_t g_split_mu = PTHREAD_MUTEX_INITIALIZER;
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

static int track_split_pid_locked(pid_t p) {
  if (p <= 0) return 0;
  if (find_split_entry_locked(p) >= 0) return 1;
  if (g_split_count >= g_split_cap) {
    int next_cap = g_split_cap == 0 ? 16 : g_split_cap * 2;
    SplitPidEntry *next = (SplitPidEntry *)realloc(g_split_entries, (size_t)next_cap * sizeof(SplitPidEntry));
    if (!next) return 0;
    g_split_entries = next;
    g_split_cap = next_cap;
  }
  g_split_entries[g_split_count].pid = p;
  g_split_entries[g_split_count].killing = 0;
  g_split_count++;
  return 1;
}

static void untrack_split_pid_locked(pid_t p) {
  int idx = find_split_entry_locked(p);
  if (idx >= 0) {
    g_split_entries[idx] = g_split_entries[--g_split_count];
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
  if (idx >= 0) {
    if (g_split_entries[idx].killing) {
      pthread_mutex_unlock(&g_split_mu);
      return 0;
    }
    g_split_entries[idx].killing = 1;
  }
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
    return 0;
  }

  kill(p, SIGTERM);
  for (int i = 0; i < 15; i++) {
    r = waitpid_nohang_retry(p, &st, &err);
    if (r == p || (r < 0 && err == ECHILD)) {
      pthread_mutex_lock(&g_split_mu);
      untrack_split_pid_locked(p);
      pthread_mutex_unlock(&g_split_mu);
      return 1;
    }
    usleep(10000);
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
  pthread_mutex_unlock(&g_split_mu);
  return 0;
}

void fv_app_cleanup_split_hosts(void) {
  pthread_mutex_lock(&g_split_mu);
  g_shutting_down = 1;
  int n = g_split_count;
  pid_t *snapshot = n > 0 ? (pid_t *)malloc((size_t)n * sizeof(pid_t)) : NULL;
  int m = 0;
  if (snapshot) {
    for (int i = 0; i < n; i++) {
      if (g_split_entries[i].pid > 0 && !g_split_entries[i].killing) {
        snapshot[m++] = g_split_entries[i].pid;
      }
    }
  }
  pthread_mutex_unlock(&g_split_mu);
  for (int i = 0; i < m; i++) {
    (void)fv_app_kill_split_host((int32_t)snapshot[i]);
  }
  if (snapshot) free(snapshot);
}

int32_t fv_app_reap_pid(int32_t pid) {
  if (pid <= 0) return 0;
  int st = 0;
  int err = 0;
  pid_t r = waitpid_nohang_retry((pid_t)pid, &st, &err);
  if (r == (pid_t)pid || (r < 0 && err == ECHILD)) {
    pthread_mutex_lock(&g_split_mu);
    untrack_split_pid_locked((pid_t)pid);
    pthread_mutex_unlock(&g_split_mu);
    return r == (pid_t)pid ? 1 : 0;
  }
  return 0;
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

  // Strip FV_PROFILE_DIR from the secondary split-pane host so it never overwrites
  // the primary browser's persisted session store on shutdown.
  int env_count = 0;
  if (environ) {
    while (environ[env_count] != NULL) env_count++;
  }
  char **child_env = (char **)malloc((size_t)(env_count + 1) * sizeof(char *));
  int ce = 0;
  if (child_env && environ) {
    for (int i = 0; i < env_count; i++) {
      if (strncmp(environ[i], "FV_PROFILE_DIR=", 15) == 0) continue;
      child_env[ce++] = environ[i];
    }
    child_env[ce] = NULL;
  }

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  pid_t pid = -1;
  char **spawn_env = child_env ? child_env : environ;
  int rc = is_path
    ? posix_spawn(&pid, host_bin, &fa, &attr, argv, spawn_env)
    : posix_spawnp(&pid, host_bin, &fa, &attr, argv, spawn_env);

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (child_env) free(child_env);
  if (url_buf) free(url_buf);

  if (rc != 0 || pid <= 0) return -1;
  pthread_mutex_lock(&g_split_mu);
  if (g_shutting_down) {
    track_split_pid_locked(pid);
    pthread_mutex_unlock(&g_split_mu);
    (void)fv_app_kill_split_host((int32_t)pid);
    return -1;
  }
  (void)track_split_pid_locked(pid);
  pthread_mutex_unlock(&g_split_mu);
  return (int32_t)pid;
}
