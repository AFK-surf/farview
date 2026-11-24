// Process-isolated secondary `pion-host` lifecycle manager for Arc Split View.
// Uses posix_spawn (safe in multi-threaded Metal/AppKit processes) + exact-PID
// SIGTERM/SIGKILL + waitpid reaping to guarantee zero zombie processes.

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
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

static pid_t g_split_host_pid = -1;
static int g_atexit_registered = 0;

int32_t fv_app_kill_split_host(int32_t pid) {
  if (pid <= 0) return 0;
  pid_t p = (pid_t)pid;
  int st = 0;
  if (waitpid(p, &st, WNOHANG) == p) {
    if (g_split_host_pid == p) g_split_host_pid = -1;
    return 1;
  }
  kill(p, SIGTERM);
  for (int i = 0; i < 15; i++) {
    if (waitpid(p, &st, WNOHANG) == p) {
      if (g_split_host_pid == p) g_split_host_pid = -1;
      return 1;
    }
    usleep(10000);
  }
  kill(p, SIGKILL);
  waitpid(p, &st, 0);
  if (g_split_host_pid == p) g_split_host_pid = -1;
  return 1;
}

static void fv_app_cleanup_split_host(void) {
  if (g_split_host_pid > 0) {
    fv_app_kill_split_host((int32_t)g_split_host_pid);
  }
}

int32_t fv_app_reap_pid(int32_t pid) {
  if (pid <= 0) return 0;
  int st = 0;
  pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
  if (r == (pid_t)pid) {
    if (g_split_host_pid == (pid_t)pid) g_split_host_pid = -1;
    return 1;
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
  if (!g_atexit_registered) {
    atexit(fv_app_cleanup_split_host);
    g_atexit_registered = 1;
  }
  if (g_split_host_pid > 0) {
    fv_app_kill_split_host((int32_t)g_split_host_pid);
  }

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

  pid_t pid = -1;
  char **spawn_env = child_env ? child_env : environ;
  int rc = is_path
    ? posix_spawn(&pid, host_bin, &fa, NULL, argv, spawn_env)
    : posix_spawnp(&pid, host_bin, &fa, NULL, argv, spawn_env);

  posix_spawn_file_actions_destroy(&fa);
  if (child_env) free(child_env);
  if (url_buf) free(url_buf);

  if (rc != 0 || pid <= 0) return -1;
  g_split_host_pid = pid;
  return (int32_t)pid;
}
