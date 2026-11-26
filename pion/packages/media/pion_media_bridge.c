#include "c_compat/moonbit.h"
#include "../../../native/page/audio_out.h"
#ifdef _WIN32
#include "../../../native/win_compat.h"
#endif
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

extern void fv_media_register_dir(moonbit_bytes_t dir);
extern void fv_media_cleanup_dir(moonbit_bytes_t dir);
extern void fv_media_unlink(moonbit_bytes_t path);
extern int32_t fv_media_append_file(moonbit_bytes_t path_bytes, moonbit_bytes_t data_bytes);
extern moonbit_bytes_t fv_media_probe(moonbit_bytes_t path_bytes);
extern moonbit_bytes_t fv_media_frame_jpeg(moonbit_bytes_t path_bytes, int32_t time_ms, int32_t scale_w);
extern int32_t fv_media_stream_open(moonbit_bytes_t path_bytes, int32_t start_ms, int32_t scale_w, int32_t fps);
extern moonbit_bytes_t fv_media_stream_read(int32_t handle);
extern int32_t fv_media_audio_open(moonbit_bytes_t path_bytes, int32_t start_ms);
extern moonbit_bytes_t fv_media_audio_read(int32_t handle, int32_t max_bytes);
extern int32_t fv_media_inproc_available(void);
extern moonbit_bytes_t fv_media_probe_mem(moonbit_bytes_t data_bytes);
extern moonbit_bytes_t fv_media_frame_jpeg_mem(moonbit_bytes_t data_bytes, int32_t time_ms, int32_t scale_w);
extern int32_t fv_media_mem_stream_open(int32_t scale_w, int32_t fps);
extern int32_t fv_media_mem_stream_append(int32_t handle, moonbit_bytes_t data_bytes);
extern void fv_media_mem_stream_end(int32_t handle);
extern moonbit_bytes_t fv_media_mem_stream_probe(int32_t handle);
extern moonbit_bytes_t fv_media_stream_read_rgba(int32_t handle);
extern int32_t fv_media_stream_last_pts_ms(int32_t handle);
extern int32_t fv_media_stream_seek(int32_t handle, int32_t target_ms);
extern int32_t fv_media_mem_audio_open(void);
extern int32_t fv_media_mem_audio_append(int32_t handle, moonbit_bytes_t data_bytes);
extern void fv_media_mem_audio_end(int32_t handle);
extern int32_t fv_media_audio_last_pts_ms(int32_t handle);
extern int32_t fv_media_audio_seek(int32_t handle, int32_t target_ms);
extern moonbit_bytes_t fv_media_decode_audio(moonbit_bytes_t data_bytes);

static moonbit_bytes_t wrap_bytes(const void *p, int32_t len) {
  moonbit_bytes_t b = moonbit_make_bytes(len, 0);
  if (len > 0 && p) memcpy(b, p, (size_t)len);
  return b;
}

int32_t pion_media_mb_len(void *b) {
  return fv_mb_bytes_len((moonbit_bytes_t)b);
}

void pion_media_mb_free(void *b) {
  fv_mb_bytes_free((moonbit_bytes_t)b);
}

void pion_media_mkdir(const void *dir_cstr) {
  if (dir_cstr) mkdir((const char *)dir_cstr, 0700);
}

void pion_media_register_dir(const void *dir, int32_t len) {
  moonbit_bytes_t b = wrap_bytes(dir, len);
  fv_media_register_dir(b);
  fv_mb_bytes_free(b);
}

void pion_media_cleanup_dir(const void *dir, int32_t len) {
  moonbit_bytes_t b = wrap_bytes(dir, len);
  fv_media_cleanup_dir(b);
  fv_mb_bytes_free(b);
}

void pion_media_unlink(const void *path, int32_t len) {
  moonbit_bytes_t b = wrap_bytes(path, len);
  fv_media_unlink(b);
  fv_mb_bytes_free(b);
}

int32_t pion_media_append_file(const void *path, int32_t plen, const void *data, int32_t dlen) {
  moonbit_bytes_t pb = wrap_bytes(path, plen);
  moonbit_bytes_t db = wrap_bytes(data, dlen);
  int32_t ok = fv_media_append_file(pb, db);
  fv_mb_bytes_free(pb);
  fv_mb_bytes_free(db);
  return ok;
}

void *pion_media_probe(const void *path, int32_t len) {
  moonbit_bytes_t pb = wrap_bytes(path, len);
  moonbit_bytes_t res = fv_media_probe(pb);
  fv_mb_bytes_free(pb);
  return (void *)res;
}

void *pion_media_frame_jpeg(const void *path, int32_t len, int32_t time_ms, int32_t scale_w) {
  moonbit_bytes_t pb = wrap_bytes(path, len);
  moonbit_bytes_t res = fv_media_frame_jpeg(pb, time_ms, scale_w);
  fv_mb_bytes_free(pb);
  return (void *)res;
}

int32_t pion_media_stream_open(const void *path, int32_t len, int32_t start_ms, int32_t scale_w, int32_t fps) {
  moonbit_bytes_t pb = wrap_bytes(path, len);
  int32_t h = fv_media_stream_open(pb, start_ms, scale_w, fps);
  fv_mb_bytes_free(pb);
  return h;
}

void *pion_media_stream_read(int32_t handle) {
  return (void *)fv_media_stream_read(handle);
}

int32_t pion_media_audio_open(const void *path, int32_t len, int32_t start_ms) {
  moonbit_bytes_t pb = wrap_bytes(path, len);
  int32_t h = fv_media_audio_open(pb, start_ms);
  fv_mb_bytes_free(pb);
  return h;
}

void *pion_media_audio_read(int32_t handle, int32_t max_bytes) {
  return (void *)fv_media_audio_read(handle, max_bytes);
}

void *pion_media_probe_mem(const void *data, int32_t len) {
  moonbit_bytes_t db = wrap_bytes(data, len);
  moonbit_bytes_t res = fv_media_probe_mem(db);
  fv_mb_bytes_free(db);
  return (void *)res;
}

void *pion_media_frame_jpeg_mem(const void *data, int32_t len, int32_t time_ms, int32_t scale_w) {
  moonbit_bytes_t db = wrap_bytes(data, len);
  moonbit_bytes_t res = fv_media_frame_jpeg_mem(db, time_ms, scale_w);
  fv_mb_bytes_free(db);
  return (void *)res;
}

int32_t pion_media_mem_stream_append(int32_t handle, const void *data, int32_t len) {
  moonbit_bytes_t db = wrap_bytes(data, len);
  int32_t ok = fv_media_mem_stream_append(handle, db);
  fv_mb_bytes_free(db);
  return ok;
}

void *pion_media_mem_stream_probe(int32_t handle) {
  return (void *)fv_media_mem_stream_probe(handle);
}

void *pion_media_stream_read_rgba(int32_t handle) {
  return (void *)fv_media_stream_read_rgba(handle);
}

int32_t pion_media_mem_audio_append(int32_t handle, const void *data, int32_t len) {
  moonbit_bytes_t db = wrap_bytes(data, len);
  int32_t ok = fv_media_mem_audio_append(handle, db);
  fv_mb_bytes_free(db);
  return ok;
}

void *pion_media_decode_audio(const void *data, int32_t len) {
  moonbit_bytes_t db = wrap_bytes(data, len);
  moonbit_bytes_t res = fv_media_decode_audio(db);
  fv_mb_bytes_free(db);
  return (void *)res;
}

extern int32_t fv_media_audio_attach_output(int32_t audio_handle, int32_t out_handle);
extern int32_t fv_media_play_decoded_audio(moonbit_bytes_t fvad_bytes, int32_t out_handle);

int32_t pion_media_audio_attach_output(int32_t audio_handle, int32_t out_handle) {
  return fv_media_audio_attach_output(audio_handle, out_handle);
}

int32_t pion_media_play_decoded_audio(const void *fvad, int32_t len, int32_t out_handle) {
  moonbit_bytes_t fb = wrap_bytes(fvad, len);
  int32_t frames = fv_media_play_decoded_audio(fb, out_handle);
  fv_mb_bytes_free(fb);
  return frames;
}

int32_t pion_audio_out_probe_hardware(void) {
  return fv_audio_out_probe_hardware();
}

int32_t pion_audio_out_open(int32_t sample_rate, int32_t channels, int32_t fmt, int32_t flags) {
  return fv_audio_out_open(sample_rate, channels, fmt, flags);
}

int32_t pion_audio_out_write(int32_t handle, const void *data, int32_t len) {
  return fv_audio_out_write(handle, (const uint8_t *)data, len);
}

int32_t pion_audio_out_play(int32_t handle) {
  return fv_audio_out_play(handle);
}

int32_t pion_audio_out_pause(int32_t handle) {
  return fv_audio_out_pause(handle);
}

int32_t pion_audio_out_set_volume(int32_t handle, double volume) {
  return fv_audio_out_set_volume(handle, (float)volume);
}

int32_t pion_audio_out_set_muted(int32_t handle, int32_t muted) {
  return fv_audio_out_set_muted(handle, muted);
}

int32_t pion_audio_out_flush(int32_t handle) {
  return fv_audio_out_flush(handle);
}

void pion_audio_out_close(int32_t handle) {
  fv_audio_out_close(handle);
}

int32_t pion_audio_out_active_count(void) {
  return fv_audio_out_active_count();
}

int32_t pion_audio_out_stats_string(int32_t handle, void *out_buf, int32_t cap) {
  return fv_audio_out_stats_string(handle, (char *)out_buf, cap);
}
