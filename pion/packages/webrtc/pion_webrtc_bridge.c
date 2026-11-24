#include "../../../native/page/webrtc_native.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const void *p, int32_t len) {
  if (len <= 0 || !p) {
    char *s = (char *)malloc(1);
    if (s) s[0] = '\0';
    return s;
  }
  char *s = (char *)malloc((size_t)len + 1);
  if (!s) return NULL;
  memcpy(s, p, (size_t)len);
  s[len] = '\0';
  return s;
}

int32_t pion_webrtc_udp_open(int32_t loopback_only) {
  return fv_webrtc_udp_open(loopback_only);
}

int32_t pion_webrtc_udp_port(int32_t handle) {
  return fv_webrtc_udp_port(handle);
}

int32_t pion_webrtc_udp_send(int32_t handle, const void *ip, int32_t ip_len,
                             int32_t port, const void *data, int32_t len) {
  char *ip_s = dup_cstr(ip, ip_len);
  if (!ip_s) return -1;
  int32_t n = fv_webrtc_udp_send(handle, ip_s, port, (const uint8_t *)data, len);
  free(ip_s);
  return n;
}

int32_t pion_webrtc_udp_recv(int32_t handle, void *out_buf, int32_t cap,
                             void *out_ip_buf, int32_t ip_cap, void *out_meta_i32) {
  int32_t port = 0;
  char *ip_buf = (char *)out_ip_buf;
  if (ip_buf && ip_cap > 0) ip_buf[0] = '\0';
  int32_t n = fv_webrtc_udp_recv(handle, (uint8_t *)out_buf, cap, ip_buf, ip_cap, &port);
  if (out_meta_i32) {
    int32_t *meta = (int32_t *)out_meta_i32;
    meta[0] = (n > 0 && ip_buf) ? (int32_t)strlen(ip_buf) : 0;
    meta[1] = (n > 0) ? port : 0;
  }
  return n;
}

void pion_webrtc_udp_close(int32_t handle) {
  fv_webrtc_udp_close(handle);
}

int32_t pion_webrtc_udp_active_count(void) {
  return fv_webrtc_udp_active_count();
}

int32_t pion_webrtc_local_ips(void *out_buf, int32_t cap) {
  return fv_webrtc_local_ips((char *)out_buf, cap);
}

int32_t pion_dtls_new(int32_t is_server) {
  return fv_dtls_new(is_server);
}

int32_t pion_dtls_set_role(int32_t handle, int32_t is_server) {
  return fv_dtls_set_role(handle, is_server);
}

void pion_dtls_close(int32_t handle) {
  fv_dtls_close(handle);
}

int32_t pion_dtls_active_count(void) {
  return fv_dtls_active_count();
}

int32_t pion_dtls_local_fingerprint(int32_t handle, void *out_buf, int32_t cap) {
  return fv_dtls_local_fingerprint(handle, (char *)out_buf, cap);
}

int32_t pion_dtls_feed(int32_t handle, const void *pkt, int32_t len) {
  return fv_dtls_feed(handle, (const uint8_t *)pkt, len);
}

int32_t pion_dtls_step(int32_t handle) {
  return fv_dtls_step(handle);
}

int32_t pion_dtls_poll_out(int32_t handle, void *out_buf, int32_t cap) {
  return fv_dtls_poll_out(handle, (uint8_t *)out_buf, cap);
}

int32_t pion_dtls_is_connected(int32_t handle) {
  return fv_dtls_is_connected(handle);
}

int32_t pion_dtls_peer_fingerprint(int32_t handle, void *out_buf, int32_t cap) {
  return fv_dtls_peer_fingerprint(handle, (char *)out_buf, cap);
}

int32_t pion_dtls_verify_peer_fingerprint(int32_t handle, const void *expected,
                                          int32_t expected_len) {
  return fv_dtls_verify_peer_fingerprint(handle, (const char *)expected, expected_len);
}

int32_t pion_dtls_export_srtp(int32_t handle, void *out_buf, int32_t cap,
                              void *out_profile_i32) {
  int32_t pid = 0;
  int32_t n = fv_dtls_export_srtp(handle, (uint8_t *)out_buf, cap, &pid);
  if (out_profile_i32) {
    ((int32_t *)out_profile_i32)[0] = pid;
  }
  return n;
}

int32_t pion_dtls_app_write(int32_t handle, const void *data, int32_t len) {
  return fv_dtls_app_write(handle, (const uint8_t *)data, len);
}

int32_t pion_dtls_app_read(int32_t handle, void *out_buf, int32_t cap) {
  return fv_dtls_app_read(handle, (uint8_t *)out_buf, cap);
}

int32_t pion_capture_enumerate_devices(void *out_buf, int32_t cap) {
  return fv_capture_enumerate_devices((char *)out_buf, cap);
}

int32_t pion_capture_device_summary(void *out_buf, int32_t cap) {
  return fv_capture_device_summary((char *)out_buf, cap);
}

int32_t pion_capture_open(int32_t kind, int32_t width, int32_t height) {
  return fv_capture_open(kind, width, height);
}

int32_t pion_capture_open_ex(int32_t kind, int32_t width, int32_t height, int32_t flags) {
  return fv_capture_open_ex(kind, width, height, flags);
}

int32_t pion_capture_read_frame(int32_t handle, void *out_buf, int32_t cap) {
  return fv_capture_read_frame(handle, (uint8_t *)out_buf, cap);
}

int32_t pion_capture_status(int32_t handle, void *out_buf, int32_t cap) {
  return fv_capture_status(handle, (char *)out_buf, cap);
}

void pion_capture_close(int32_t handle) {
  fv_capture_close(handle);
}

int32_t pion_capture_active_count(void) {
  return fv_capture_active_count();
}

int32_t pion_webrtc_audio_out_open(int32_t sample_rate, int32_t channels, int32_t flags) {
  return fv_webrtc_audio_out_open(sample_rate, channels, flags);
}

int32_t pion_webrtc_audio_out_write(int32_t handle, const void *pcm, int32_t len) {
  return fv_webrtc_audio_out_write(handle, (const uint8_t *)pcm, len);
}

void pion_webrtc_audio_out_close(int32_t handle) {
  fv_webrtc_audio_out_close(handle);
}

int32_t pion_webrtc_random_bytes(void *out_buf, int32_t len) {
  return fv_webrtc_random_bytes((uint8_t *)out_buf, len);
}
