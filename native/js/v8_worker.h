#ifndef FARVIEW_NATIVE_JS_V8_WORKER_H
#define FARVIEW_NATIVE_JS_V8_WORKER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <v8.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of concurrent OS-thread V8 Isolate workers.
#define FV_MAX_WORKERS 16

// Message kinds returned by fv_worker_poll.
#define FV_WORKER_MSG_NONE  0
#define FV_WORKER_MSG_DATA  1
#define FV_WORKER_MSG_ERROR 2

// Ensure global V8 platform is initialized (implemented in v8_shim.cc).
void* fv_v8_ensure_platform(void);

// Pump one foreground V8 platform task for `isolate` (returns non-zero if a task ran).
int32_t fv_v8_pump_message_loop(void* platform, void* isolate);

// Spawn a dedicated OS thread with an independent v8::Isolate and DedicatedWorkerGlobalScope.
// Returns worker id in [1, FV_MAX_WORKERS] on success, or <= 0 on failure.
int32_t fv_worker_spawn(const char* script_utf8, int32_t script_len,
                        const char* url_utf8, int32_t url_len);

// Post a UTF-8 message from C/Pion into worker `id`'s inbound queue.
// Returns 1 on success, 0 on failure.
int32_t fv_worker_post(int32_t id, const char* data_utf8, int32_t data_len);

// Non-blocking poll of worker `id`'s outbound queue.
// Returns FV_WORKER_MSG_NONE (0), FV_WORKER_MSG_DATA (1), or FV_WORKER_MSG_ERROR (2).
// When non-zero, allocates a NUL-terminated UTF-8 copy in *out_utf8 (caller frees via fv_worker_free)
// and sets *out_len to its byte length.
int32_t fv_worker_poll(int32_t id, char** out_utf8, int32_t* out_len);

// Free a buffer returned by fv_worker_poll.
void fv_worker_free(void* p);

// Terminate worker `id`: interrupts V8 execution, wakes queue, joins OS thread, disposes Isolate.
void fv_worker_terminate(int32_t id);

// Terminate all active workers (also registered via atexit).
void fv_worker_terminate_all(void);

// Number of currently active OS-thread workers.
int32_t fv_worker_active_count(void);

// OS thread ID of worker `id` (0 if not active / not yet started).
uint64_t fv_worker_thread_id(int32_t id);

// OS thread ID of the calling thread (main thread when called from main).
uint64_t fv_worker_main_thread_id(void);

#ifdef __cplusplus
}

// Install main-isolate JS bindings (__fvWorkerSpawn, __fvWorkerPost, __fvWorkerPoll,
// __fvWorkerTerminate, __fvWorkerActiveCount, __fvWorkerMainTid, __fvWorkerThreadId)
// with full v8::ValueSerializer / ValueDeserializer + Transferable ArrayBuffer support.
void fv_worker_install_main_bindings(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
#endif

#endif // FARVIEW_NATIVE_JS_V8_WORKER_H
