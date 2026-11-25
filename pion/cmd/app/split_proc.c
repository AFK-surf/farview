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
#include <time.h>
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

static int fv_resolve_home_path(const char *path_ptr, int32_t path_len, char *out, size_t out_cap) {
  if (!path_ptr || path_len <= 0 || (size_t)path_len >= out_cap) return -1;
  char raw[4096];
  if ((size_t)path_len >= sizeof(raw)) return -1;
  memcpy(raw, path_ptr, (size_t)path_len);
  raw[path_len] = '\0';
  if (raw[0] == '~' && (raw[1] == '/' || raw[1] == '\0')) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return -1;
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

int32_t fv_app_file_exists(const char *path_ptr, int32_t path_len) {
  char resolved[4096];
  if (fv_resolve_home_path(path_ptr, path_len, resolved, sizeof(resolved)) != 0) return 0;
  return (access(resolved, F_OK) == 0) ? 1 : 0;
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
  if (access(resolved, F_OK) != 0) return -1;

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
