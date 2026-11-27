#ifndef FARVIEW_NATIVE_PAGE_NATIVE_MESSAGING_H
#define FARVIEW_NATIVE_PAGE_NATIVE_MESSAGING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Chrome Native Messaging hard limit for messages from native host to browser (1 MiB).
#define FV_NM_MAX_INCOMING_BYTES (1024 * 1024)
// Max buffered outgoing bytes per port (4 MiB bounded backpressure queue).
#define FV_NM_MAX_OUTGOING_BYTES (4 * 1024 * 1024)
// Max concurrent native messaging host child processes.
#define FV_NM_MAX_PORTS 16

// Spawns a Chrome Native Messaging host executable with `origin` as argv[1] (when non-empty).
// Returns a generation-tagged handle > 0 on success, or <= 0 on failure.
int32_t fv_nm_spawn(const char* exe_path, int32_t exe_len,
                    const char* origin, int32_t origin_len);

// Enqueues a UTF-8 JSON message framed with a 4-byte native-endian uint32 length prefix
// and performs a non-blocking flush. Never stalls the calling thread.
// Returns 1 on success, 0 on failure (slot closed on fatal pipe error).
int32_t fv_nm_write(int32_t handle, const uint8_t* json_utf8, int32_t json_len);

// Non-blocking pump + peek: flushes pending outgoing writes and reads incoming bytes.
// Returns:
//   > 0: exact byte length of a complete ready message (call fv_nm_poll to read it)
//     0: no complete message ready yet (child still alive)
//    -1: EOF, child exited, invalid/stale handle, or protocol violation (port closed)
int32_t fv_nm_peek_len(int32_t handle);

// Non-blocking poll for the next complete length-prefixed JSON message from the host.
// Returns:
//   > 0: byte length of the received JSON message written to `out_buf`
//     0: no complete message ready yet (child still alive)
//    -1: EOF, child exited, invalid/stale handle, or protocol violation (port closed)
//    -2: `out_cap` is smaller than the ready message (message retained; call fv_nm_peek_len)
int32_t fv_nm_poll(int32_t handle, uint8_t* out_buf, int32_t out_cap);

// Closes pipes and terminates the native messaging host process for `handle`.
void fv_nm_close(int32_t handle);

// Closes all active native messaging ports (also registered with atexit).
void fv_nm_close_all(void);

// Returns the number of currently active native messaging ports.
int32_t fv_nm_active_count(void);

// Returns current real wall-clock Unix epoch time in milliseconds (UTC).
int64_t fv_nm_epoch_ms(void);

// On Windows, looks up the Native Messaging host manifest path for `app_name` in
// HKCU/HKLM Chrome/Chromium/Edge registry keys. On POSIX returns 0.
// Returns byte length written to `out_buf`, 0 if not found, or < 0 on buffer overflow.
int32_t fv_nm_lookup_win_registry(const char* app_name, int32_t app_len,
                                  uint8_t* out_buf, int32_t out_cap);

// Recursively walks `root_dir`, rejecting symlinks, and writes NUL-separated
// relative POSIX paths into `out_buf` (capped at `max_files`).
// Returns byte length written (>= 0), -1 on invalid args/error, or -2 if truncated/cap exceeded.
int32_t fv_ext_walk_dir(const char* root_dir, int32_t root_len,
                        uint8_t* out_buf, int32_t out_cap, int32_t max_files);

#ifdef __cplusplus
}
#endif

#endif // FARVIEW_NATIVE_PAGE_NATIVE_MESSAGING_H
