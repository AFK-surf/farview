#ifndef FV_WEBRTC_NATIVE_H
#define FV_WEBRTC_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Non-blocking POSIX UDP socket pool & local IP discovery --- */

int32_t fv_webrtc_udp_open(int32_t loopback_only);
int32_t fv_webrtc_udp_port(int32_t handle);
int32_t fv_webrtc_udp_send(int32_t handle, const char *ip, int32_t port,
                           const uint8_t *data, int32_t len);
int32_t fv_webrtc_udp_recv(int32_t handle, uint8_t *out_buf, int32_t cap,
                           char *out_ip, int32_t ip_cap, int32_t *out_port);
void fv_webrtc_udp_close(int32_t handle);
int32_t fv_webrtc_udp_active_count(void);
int32_t fv_webrtc_local_ips(char *out_buf, int32_t max_len);

/* --- Real RFC 5764 BoringSSL DTLS 1.2 + SRTP keying material export --- */

#define FV_SRTP_PROFILE_AES128_CM_SHA1_80 1
#define FV_SRTP_PROFILE_AEAD_AES_128_GCM  7

int32_t fv_dtls_new(int32_t is_server);
int32_t fv_dtls_set_role(int32_t handle, int32_t is_server);
void fv_dtls_close(int32_t handle);
int32_t fv_dtls_active_count(void);
int32_t fv_dtls_local_fingerprint(int32_t handle, char *out_buf, int32_t cap);
int32_t fv_dtls_feed(int32_t handle, const uint8_t *pkt, int32_t len);
int32_t fv_dtls_step(int32_t handle);
int32_t fv_dtls_poll_out(int32_t handle, uint8_t *out_buf, int32_t cap);
int32_t fv_dtls_is_connected(int32_t handle);
int32_t fv_dtls_peer_fingerprint(int32_t handle, char *out_buf, int32_t cap);
int32_t fv_dtls_verify_peer_fingerprint(int32_t handle, const char *expected_fp,
                                        int32_t expected_len);
int32_t fv_dtls_export_srtp(int32_t handle, uint8_t *out_buf, int32_t cap,
                            int32_t *out_profile_id);
int32_t fv_dtls_app_write(int32_t handle, const uint8_t *data, int32_t len);
int32_t fv_dtls_app_read(int32_t handle, uint8_t *out_buf, int32_t cap);

/* --- OS audio/video/display capture device enumeration & frame pipeline --- */

#define FV_CAPTURE_AUDIO  0
#define FV_CAPTURE_VIDEO  1
#define FV_CAPTURE_SCREEN 2

int32_t fv_capture_enumerate_devices(char *out_buf, int32_t cap);
int32_t fv_capture_device_summary(char *out_buf, int32_t cap);
int32_t fv_capture_open(int32_t kind, int32_t width, int32_t height);
int32_t fv_capture_open_ex(int32_t kind, int32_t width, int32_t height, int32_t flags);
int32_t fv_capture_read_frame(int32_t handle, uint8_t *out_buf, int32_t cap);
int32_t fv_capture_status(int32_t handle, char *out_buf, int32_t cap);
void fv_capture_close(int32_t handle);
int32_t fv_capture_active_count(void);

/* --- Shared real-time audio output hook for WebRTC remote audio tracks --- */
int32_t fv_webrtc_audio_out_open(int32_t sample_rate, int32_t channels, int32_t flags);
int32_t fv_webrtc_audio_out_write(int32_t handle, const uint8_t *pcm, int32_t len);
void fv_webrtc_audio_out_close(int32_t handle);

/* --- Cryptographic random bytes (BoringSSL RAND_bytes) --- */
int32_t fv_webrtc_random_bytes(uint8_t *out_buf, int32_t len);

#ifdef __cplusplus
}
#endif

#endif /* FV_WEBRTC_NATIVE_H */
