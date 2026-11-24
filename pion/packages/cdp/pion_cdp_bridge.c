#include "../../../native/page/cdp_ws.h"
#include <stdint.h>

int32_t pion_cdp_listen(int32_t port) {
  return fv_cdp_listen(port);
}

int32_t pion_cdp_port(int32_t handle) {
  return fv_cdp_port(handle);
}

int32_t pion_cdp_set_target_meta(int32_t handle,
                                 const void *title, int32_t title_len,
                                 const void *url, int32_t url_len) {
  return fv_cdp_set_target_meta(handle, title, title_len, url, url_len);
}

int32_t pion_cdp_poll(int32_t handle, void *out_buf, int32_t cap, void *out_client_i32) {
  int32_t cid = 0;
  int32_t n = fv_cdp_poll(handle, out_buf, cap, &cid);
  if (out_client_i32) {
    ((int32_t *)out_client_i32)[0] = cid;
  }
  return n;
}

int32_t pion_cdp_send(int32_t handle, int32_t client_id, const void *json, int32_t len) {
  return fv_cdp_send(handle, client_id, json, len);
}

void pion_cdp_close(int32_t handle) {
  fv_cdp_close(handle);
}

int32_t pion_cdp_active_count(void) {
  return fv_cdp_active_count();
}

int32_t pion_cdp_ws_accept_key(const void *key, int32_t key_len, void *out_buf, int32_t cap) {
  return fv_cdp_ws_accept_key(key, key_len, out_buf, cap);
}

int32_t pion_cdp_client_connect(int32_t port) {
  return fv_cdp_client_connect(port);
}

int32_t pion_cdp_client_send_raw(int32_t fd, const void *data, int32_t len) {
  return fv_cdp_client_send_raw(fd, data, len);
}

int32_t pion_cdp_client_recv_raw(int32_t fd, void *out_buf, int32_t cap, int32_t timeout_ms) {
  return fv_cdp_client_recv_raw(fd, out_buf, cap, timeout_ms);
}

void pion_cdp_client_close(int32_t fd) {
  fv_cdp_client_close(fd);
}
