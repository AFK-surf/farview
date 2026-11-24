#ifndef FARVIEW_NATIVE_PAGE_SANDBOX_H
#define FARVIEW_NATIVE_PAGE_SANDBOX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* profile_dir;
  const char* state_dir;
  const char* download_dir;
  const char* tmp_dir;
  int allow_network;
} fv_sandbox_config_t;

// Returns 1 if OS-level sandboxing is supported on this platform.
int32_t fv_sandbox_supported(void);

// Apply OS-level sandbox to the current process.
// Returns 0 on success, -1 on failure (writes NUL-terminated error into err_buf if provided).
int32_t fv_sandbox_apply(const fv_sandbox_config_t* cfg, char* err_buf, size_t err_cap);

// Convenience FFI wrapper taking length-delimited UTF-8 strings from Pion.
int32_t fv_sandbox_apply_str(const char* profile_dir, int32_t profile_len,
                             const char* state_dir, int32_t state_len,
                             const char* download_dir, int32_t download_len,
                             const char* tmp_dir, int32_t tmp_len,
                             int32_t allow_network);

// Fork a child process, apply the OS sandbox allowing read/write only inside `allowed_dir`,
// verify reading `denied_path` is rejected by the kernel (EPERM on macOS Seatbelt),
// and verify writing + reading a file inside `allowed_dir` succeeds.
// Parent waits for child via waitpid(); never alters the parent process's own privileges.
// Returns 0 on full pass, or a negative error code on failure.
int32_t fv_sandbox_self_test(const char* allowed_dir, int32_t allowed_len,
                             const char* denied_path, int32_t denied_len);

#ifdef __cplusplus
}
#endif

#endif // FARVIEW_NATIVE_PAGE_SANDBOX_H
