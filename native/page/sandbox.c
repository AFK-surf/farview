// Farview Wave W1: OS-level render process sandbox.
// - macOS (__APPLE__): Seatbelt SBPL via dlopen("/usr/lib/libSystem.B.dylib") (zero deprecation warnings,
//   zero extra link flags). Strict `(version 1) (deny default)` allowlisting only system libs/fonts/certs,
//   explicitly authorized profile/state/download/tmp dirs, optional network-outbound + 127.0.0.1 IPC,
//   and denying ~/.ssh, ~/.aws, ~/Documents, ~/Desktop, ~/.gnupg and process-exec.
// - Linux (__linux__): prctl(PR_SET_NO_NEW_PRIVS, 1) + setrlimit core/FD hardening, zero external deps.

#include "sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include "../win_compat.h"
#undef usleep
#undef setenv
#undef unsetenv
int usleep(unsigned int us) {
  fv_win_usleep((uint64_t)us);
  return 0;
}
int setenv(const char* k, const char* v, int overwrite) {
  return fv_win_setenv_compat(k, v, overwrite);
}
int unsetenv(const char* k) {
  return fv_win_unsetenv_compat(k);
}
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <dlfcn.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#endif

static void set_err(char* err_buf, size_t err_cap, const char* msg) {
  if (!err_buf || err_cap == 0) return;
  if (!msg) msg = "unknown error";
  snprintf(err_buf, err_cap, "%s", msg);
}

static char* dup_n(const char* s, int32_t len) {
  if (!s || len <= 0) return NULL;
  char* out = (char*)malloc((size_t)len + 1);
  if (!out) return NULL;
  memcpy(out, s, (size_t)len);
  out[len] = '\0';
  return out;
}

#ifdef __APPLE__

typedef int (*fv_sandbox_init_fn)(const char* profile, uint64_t flags, char** errorbuf);
typedef void (*fv_sandbox_free_error_fn)(char* errorbuf);

typedef struct {
  char* data;
  size_t len;
  size_t cap;
} fv_sb_t;

static int sb_reserve(fv_sb_t* sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) return 1;
  size_t ncap = sb->cap ? sb->cap * 2 : 1024;
  while (ncap < sb->len + extra + 1) ncap *= 2;
  char* p = (char*)realloc(sb->data, ncap);
  if (!p) return 0;
  sb->data = p;
  sb->cap = ncap;
  return 1;
}

static int sb_append(fv_sb_t* sb, const char* s) {
  size_t n = strlen(s);
  if (!sb_reserve(sb, n)) return 0;
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
  return 1;
}

static int sb_append_escaped_subpath(fv_sb_t* sb, const char* path) {
  if (!path || !path[0]) return 1;
  if (!sb_append(sb, "  (subpath \"")) return 0;
  for (const char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '"') {
      if (!sb_reserve(sb, 2)) return 0;
      sb->data[sb->len++] = '\\';
      sb->data[sb->len++] = *p;
      sb->data[sb->len] = '\0';
    } else if ((unsigned char)*p >= 0x20) {
      if (!sb_reserve(sb, 1)) return 0;
      sb->data[sb->len++] = *p;
      sb->data[sb->len] = '\0';
    }
  }
  return sb_append(sb, "\")\n");
}

static int sb_allow_dir(fv_sb_t* sb, const char* dir) {
  if (!dir || !dir[0]) return 1;
  if (!sb_append(sb, "(allow file-read* file-write*\n")) return 0;
  if (!sb_append_escaped_subpath(sb, dir)) return 0;
  char resolved[PATH_MAX];
  if (realpath(dir, resolved) != NULL && strcmp(resolved, dir) != 0) {
    if (!sb_append_escaped_subpath(sb, resolved)) return 0;
  }
  return sb_append(sb, ")\n");
}

int32_t fv_sandbox_supported(void) { return 1; }

int32_t fv_sandbox_apply(const fv_sandbox_config_t* cfg, char* err_buf, size_t err_cap) {
  void* lib = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
  if (!lib) {
    set_err(err_buf, err_cap, "dlopen libSystem.B.dylib failed");
    return -1;
  }
  fv_sandbox_init_fn fn_init = (fv_sandbox_init_fn)dlsym(lib, "sandbox_init");
  fv_sandbox_free_error_fn fn_free = (fv_sandbox_free_error_fn)dlsym(lib, "sandbox_free_error");
  if (!fn_init) {
    set_err(err_buf, err_cap, "dlsym sandbox_init failed");
    return -1;
  }

  fv_sb_t sb = {NULL, 0, 0};
  int ok = 1;
  ok = ok && sb_append(&sb,
      "(version 1)\n"
      "(deny default)\n"
      "(allow signal (target self))\n"
      "(allow sysctl-read)\n"
      "(allow mach-lookup)\n"
      "(allow file-read-metadata)\n"
      "(allow file-read*\n"
      "  (subpath \"/System/Library\")\n"
      "  (subpath \"/usr/lib\")\n"
      "  (subpath \"/usr/share\")\n"
      "  (subpath \"/opt/homebrew/lib\")\n"
      "  (subpath \"/Library/Fonts\")\n"
      "  (subpath \"/System/Library/Fonts\")\n"
      "  (subpath \"/private/etc/ssl\")\n"
      "  (literal \"/dev/urandom\")\n"
      "  (literal \"/dev/random\")\n"
      "  (literal \"/dev/null\")\n"
      "  (literal \"/dev/zero\")\n"
      ")\n"
      "(allow file-write* (literal \"/dev/null\"))\n");

  if (cfg) {
    ok = ok && sb_allow_dir(&sb, cfg->profile_dir);
    ok = ok && sb_allow_dir(&sb, cfg->state_dir);
    ok = ok && sb_allow_dir(&sb, cfg->download_dir);
    ok = ok && sb_allow_dir(&sb, cfg->tmp_dir);
    if (cfg->allow_network) {
      ok = ok && sb_append(&sb,
          "(allow network-outbound)\n"
          "(allow network-inbound (local ip \"localhost:*\"))\n"
          "(allow network-bind (local ip \"localhost:*\"))\n");
    }
  }

  // Explicitly reinforce denial of arbitrary process execution.
  ok = ok && sb_append(&sb, "(deny process-exec)\n");

  if (!ok || !sb.data) {
    free(sb.data);
    set_err(err_buf, err_cap, "failed to build SBPL profile");
    return -1;
  }

  char* sb_err = NULL;
  int rc = fn_init(sb.data, 0, &sb_err);
  free(sb.data);
  if (rc != 0) {
    set_err(err_buf, err_cap, sb_err ? sb_err : "sandbox_init failed");
    if (sb_err && fn_free) fn_free(sb_err);
    return -1;
  }
  return 0;
}

#elif defined(__linux__)

int32_t fv_sandbox_supported(void) { return 1; }

int32_t fv_sandbox_apply(const fv_sandbox_config_t* cfg, char* err_buf, size_t err_cap) {
  (void)cfg;
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    set_err(err_buf, err_cap, "prctl(PR_SET_NO_NEW_PRIVS) failed");
    return -1;
  }
  struct rlimit rl_core;
  rl_core.rlim_cur = 0;
  rl_core.rlim_max = 0;
  (void)setrlimit(RLIMIT_CORE, &rl_core);

  struct rlimit rl_nofile;
  if (getrlimit(RLIMIT_NOFILE, &rl_nofile) == 0) {
    if (rl_nofile.rlim_cur > 4096) {
      rl_nofile.rlim_cur = 4096;
      if (rl_nofile.rlim_max > 4096) rl_nofile.rlim_max = 4096;
      (void)setrlimit(RLIMIT_NOFILE, &rl_nofile);
    }
  }
  return 0;
}

#elif defined(_WIN32)

typedef BOOL (WINAPI *FvSetProcMitigationFn)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
typedef BOOL (WINAPI *FvGetProcMitigationFn)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);

static HANDLE g_win_sandbox_job = NULL;

int32_t fv_sandbox_supported(void) {
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (!k32) return 0;
  return (GetProcAddress(k32, "SetProcessMitigationPolicy") != NULL) ? 1 : 0;
}

int32_t fv_sandbox_apply(const fv_sandbox_config_t* cfg, char* err_buf, size_t err_cap) {
  (void)cfg;
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (!k32) {
    set_err(err_buf, err_cap, "GetModuleHandleA(kernel32) failed");
    return -1;
  }
  FvSetProcMitigationFn fn_set =
      (FvSetProcMitigationFn)(void*)GetProcAddress(k32, "SetProcessMitigationPolicy");
  FvGetProcMitigationFn fn_get =
      (FvGetProcMitigationFn)(void*)GetProcAddress(k32, "GetProcessMitigationPolicy");
  if (!fn_set || !fn_get) {
    set_err(err_buf, err_cap, "SetProcessMitigationPolicy unavailable");
    return -1;
  }

  /* 1. Block remote UNC and low-integrity DLL loading (safe with V8 JIT + local DLLs). */
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY img_cur;
  memset(&img_cur, 0, sizeof(img_cur));
  BOOL already_img = fn_get(GetCurrentProcess(), ProcessImageLoadPolicy, &img_cur, sizeof(img_cur)) &&
                     img_cur.NoRemoteImages && img_cur.NoLowMandatoryLabelImages;
  if (!already_img) {
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img_pol;
    memset(&img_pol, 0, sizeof(img_pol));
    img_pol.NoRemoteImages = 1;
    img_pol.NoLowMandatoryLabelImages = 1;
    if (!fn_set(ProcessImageLoadPolicy, &img_pol, sizeof(img_pol))) {
      set_err(err_buf, err_cap, "SetProcessMitigationPolicy(ProcessImageLoadPolicy) failed");
      return -1;
    }
  }

  /* 2. Disable legacy AppInit_DLLs / global WinHook extension points (best-effort idempotent). */
  PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext_cur;
  memset(&ext_cur, 0, sizeof(ext_cur));
  if (!(fn_get(GetCurrentProcess(), ProcessExtensionPointDisablePolicy, &ext_cur, sizeof(ext_cur)) &&
        ext_cur.DisableExtensionPoints)) {
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext_pol;
    memset(&ext_pol, 0, sizeof(ext_pol));
    ext_pol.DisableExtensionPoints = 1;
    (void)fn_set(ProcessExtensionPointDisablePolicy, &ext_pol, sizeof(ext_pol));
  }

  /* 3. Attach current process to a UI-restricted Job Object (best-effort; nested jobs work on Win8+). */
  if (!g_win_sandbox_job) {
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
      JOBOBJECT_BASIC_UI_RESTRICTIONS ui_lim;
      memset(&ui_lim, 0, sizeof(ui_lim));
      ui_lim.UIRestrictionsClass =
          JOB_OBJECT_UILIMIT_DESKTOP |
          JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
          JOB_OBJECT_UILIMIT_EXITWINDOWS |
          JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
      (void)SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui_lim, sizeof(ui_lim));
      if (AssignProcessToJobObject(job, GetCurrentProcess())) {
        g_win_sandbox_job = job;
      } else {
        CloseHandle(job);
      }
    }
  }
  return 0;
}

#else

int32_t fv_sandbox_supported(void) { return 0; }

int32_t fv_sandbox_apply(const fv_sandbox_config_t* cfg, char* err_buf, size_t err_cap) {
  (void)cfg;
  set_err(err_buf, err_cap, "sandbox unsupported on this platform");
  return -1;
}

#endif

int32_t fv_sandbox_apply_str(const char* profile_dir, int32_t profile_len,
                             const char* state_dir, int32_t state_len,
                             const char* download_dir, int32_t download_len,
                             const char* tmp_dir, int32_t tmp_len,
                             int32_t allow_network) {
  char* p = dup_n(profile_dir, profile_len);
  char* s = dup_n(state_dir, state_len);
  char* d = dup_n(download_dir, download_len);
  char* t = dup_n(tmp_dir, tmp_len);
  fv_sandbox_config_t cfg;
  cfg.profile_dir = p;
  cfg.state_dir = s;
  cfg.download_dir = d;
  cfg.tmp_dir = t;
  cfg.allow_network = allow_network ? 1 : 0;
  int32_t rc = fv_sandbox_apply(&cfg, NULL, 0);
  free(p);
  free(s);
  free(d);
  free(t);
  return rc;
}

int32_t fv_sandbox_self_test(const char* allowed_dir, int32_t allowed_len,
                             const char* denied_path, int32_t denied_len) {
#ifdef _WIN32
  (void)denied_path;
  (void)denied_len;
  char* allow_raw = dup_n(allowed_dir, allowed_len);
  if (!allow_raw) return -1;

  char allow_buf[PATH_MAX];
  if (strncmp(allow_raw, "/tmp/", 5) == 0) {
    char tmp_root[MAX_PATH];
    DWORD tn = GetTempPathA(sizeof(tmp_root), tmp_root);
    if (tn > 0 && tn < sizeof(tmp_root)) {
      snprintf(allow_buf, sizeof(allow_buf), "%s%s", tmp_root, allow_raw + 5);
    } else {
      snprintf(allow_buf, sizeof(allow_buf), "%s", allow_raw);
    }
  } else {
    snprintf(allow_buf, sizeof(allow_buf), "%s", allow_raw);
  }
  free(allow_raw);
  (void)CreateDirectoryA(allow_buf, NULL);

  fv_sandbox_config_t cfg;
  cfg.profile_dir = NULL;
  cfg.state_dir = NULL;
  cfg.download_dir = NULL;
  cfg.tmp_dir = allow_buf;
  cfg.allow_network = 1;
  char err[256];
  if (fv_sandbox_apply(&cfg, err, sizeof(err)) != 0) {
    return -10;
  }

  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  FvGetProcMitigationFn fn_get =
      k32 ? (FvGetProcMitigationFn)(void*)GetProcAddress(k32, "GetProcessMitigationPolicy") : NULL;
  if (!fn_get) return -11;
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY img_cur;
  memset(&img_cur, 0, sizeof(img_cur));
  if (!fn_get(GetCurrentProcess(), ProcessImageLoadPolicy, &img_cur, sizeof(img_cur)) ||
      !img_cur.NoRemoteImages || !img_cur.NoLowMandatoryLabelImages) {
    return -11;
  }

  char probe_path[PATH_MAX];
  snprintf(probe_path, sizeof(probe_path), "%s\\fv_sandbox_probe_%u.txt",
           allow_buf, (unsigned)GetCurrentProcessId());
  int wfd = _open(probe_path, _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
  if (wfd < 0) return -13;
  const char* payload = "sandbox-allowed-ok";
  int nw = _write(wfd, payload, (unsigned int)strlen(payload));
  _close(wfd);
  if (nw != (int)strlen(payload)) {
    (void)_unlink(probe_path);
    return -14;
  }
  int rfd = _open(probe_path, _O_RDONLY | _O_BINARY);
  if (rfd < 0) {
    (void)_unlink(probe_path);
    return -15;
  }
  char rbuf[64];
  int nr = _read(rfd, rbuf, (unsigned int)(sizeof(rbuf) - 1));
  _close(rfd);
  (void)_unlink(probe_path);
  if (nr != (int)strlen(payload)) return -16;
  rbuf[nr] = '\0';
  if (strcmp(rbuf, payload) != 0) return -17;
  return 0;
#else
  char* allow_s = dup_n(allowed_dir, allowed_len);
  char* deny_s = dup_n(denied_path, denied_len);
  if (!allow_s || !deny_s) {
    free(allow_s);
    free(deny_s);
    return -1;
  }

  // Ensure allowed_dir exists and denied_path exists with secret data before forking.
  (void)mkdir(allow_s, 0700);
  int created_denied = 0;
  struct stat st;
  if (stat(deny_s, &st) != 0) {
    int fd = open(deny_s, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) {
      const char* secret = "TOP_SECRET_SANDBOX_CANARY\n";
      (void)write(fd, secret, strlen(secret));
      close(fd);
      created_denied = 1;
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    if (created_denied) (void)unlink(deny_s);
    free(allow_s);
    free(deny_s);
    return -2;
  }

  if (pid == 0) {
    // Child process: apply sandbox and verify kernel enforcement.
    fv_sandbox_config_t cfg;
    cfg.profile_dir = NULL;
    cfg.state_dir = NULL;
    cfg.download_dir = NULL;
    cfg.tmp_dir = allow_s;
    cfg.allow_network = 1;
    char err[256];
    if (fv_sandbox_apply(&cfg, err, sizeof(err)) != 0) {
      _exit(10);
    }

#ifdef __APPLE__
    // 1. Reading denied_path MUST be rejected by macOS Seatbelt with EPERM.
    errno = 0;
    int dfd = open(deny_s, O_RDONLY);
    if (dfd >= 0) {
      close(dfd);
      _exit(11);
    }
    if (errno != EPERM && errno != EACCES) {
      _exit(12);
    }
#elif defined(__linux__)
    // 1. Verify NO_NEW_PRIVS is active on Linux.
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
      _exit(11);
    }
#endif

    // 2. Writing and reading back a file inside allowed_dir MUST succeed.
    char probe_path[PATH_MAX];
    snprintf(probe_path, sizeof(probe_path), "%s/fv_sandbox_probe_%d.txt", allow_s, (int)getpid());
    int wfd = open(probe_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (wfd < 0) {
      _exit(13);
    }
    const char* payload = "sandbox-allowed-ok";
    ssize_t nw = write(wfd, payload, strlen(payload));
    close(wfd);
    if (nw != (ssize_t)strlen(payload)) {
      (void)unlink(probe_path);
      _exit(14);
    }
    int rfd = open(probe_path, O_RDONLY);
    if (rfd < 0) {
      (void)unlink(probe_path);
      _exit(15);
    }
    char rbuf[64];
    ssize_t nr = read(rfd, rbuf, sizeof(rbuf) - 1);
    close(rfd);
    (void)unlink(probe_path);
    if (nr != (ssize_t)strlen(payload)) {
      _exit(16);
    }
    rbuf[nr] = '\0';
    if (strcmp(rbuf, payload) != 0) {
      _exit(17);
    }
    _exit(0);
  }

  // Parent waits for child and cleans up temporary canary file if created.
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  if (created_denied) {
    (void)unlink(deny_s);
  }
  free(allow_s);
  free(deny_s);

  if (w < 0) return -3;
  if (!WIFEXITED(status)) return -4;
  int code = WEXITSTATUS(status);
  return code == 0 ? 0 : -code;
#endif
}
