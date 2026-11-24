#ifndef FV_CDP_WS_H
#define FV_CDP_WS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Real Chrome DevTools Protocol (CDP) loopback-only TCP/HTTP + RFC 6455
 * WebSocket server. Uses only standard POSIX sockets and zero external
 * libraries (pure C SHA-1 + Base64 for Sec-WebSocket-Accept).
 */

/* Bind 127.0.0.1:<port> (port=0 picks ephemeral port). Returns handle >0 or -1. */
int32_t fv_cdp_listen(int32_t port);

/* Return actual bound TCP port (>0) for handle, or -1 if invalid. */
int32_t fv_cdp_port(int32_t handle);

/* Update target metadata served by GET /json/list and GET /json. */
int32_t fv_cdp_set_target_meta(int32_t handle,
                               const void *title, int32_t title_len,
                               const void *url, int32_t url_len);

/*
 * Non-blocking poll: accepts new loopback clients, serves HTTP discovery
 * endpoints (/json/version, /json/list, /json), completes RFC 6455 WebSocket
 * handshakes, handles PING->PONG and CLOSE frames, and returns the next
 * complete client WebSocket text/binary message in out_buf.
 * Returns:
 *   > 0 : payload length written to out_buf, *out_client_id set to client id
 *     0 : no complete message ready right now
 *   < 0 : invalid handle or error
 */
int32_t fv_cdp_poll(int32_t handle, void *out_buf, int32_t cap, int32_t *out_client_id);

/* Send a WebSocket TEXT frame (RFC 6455) to client_id on server handle. */
int32_t fv_cdp_send(int32_t handle, int32_t client_id, const void *json, int32_t len);

/* Close server and all connected clients. */
void fv_cdp_close(int32_t handle);

/* Count of currently active CDP servers + connected client sockets (0 when all closed). */
int32_t fv_cdp_active_count(void);

/* Pure C RFC 6455 Sec-WebSocket-Accept helper (for tests & verification). */
int32_t fv_cdp_ws_accept_key(const void *key, int32_t key_len, void *out_buf, int32_t cap);

/* Loopback TCP client helpers for real E2E tests (127.0.0.1 only). */
int32_t fv_cdp_client_connect(int32_t port);
int32_t fv_cdp_client_send_raw(int32_t fd, const void *data, int32_t len);
int32_t fv_cdp_client_recv_raw(int32_t fd, void *out_buf, int32_t cap, int32_t timeout_ms);
void    fv_cdp_client_close(int32_t fd);

#ifdef __cplusplus
}
#endif

#endif /* FV_CDP_WS_H */
