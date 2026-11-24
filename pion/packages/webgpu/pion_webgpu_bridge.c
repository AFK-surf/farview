#include "../../../native/page/webgpu_metal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const void *p, int32_t len) {
  if (!p || len < 0) return NULL;
  char *s = (char *)malloc((size_t)len + 1u);
  if (!s) return NULL;
  if (len > 0) memcpy(s, p, (size_t)len);
  s[len] = '\0';
  return s;
}

int32_t pion_wgpu_available(void) {
  return fv_wgpu_available();
}

int32_t pion_wgpu_require_metal(void) {
  const char *v = getenv("FV_REQUIRE_METAL");
  return (v && v[0] == '1') ? 1 : 0;
}

void pion_wgpu_log_skip(void) {
  fprintf(stderr, "[webgpu_test] skipping Metal GPU test: Metal unavailable on this host (set FV_REQUIRE_METAL=1 to require)\n");
}

int32_t pion_wgpu_create_device(void) {
  return fv_wgpu_create_device();
}

void pion_wgpu_destroy_device(int32_t dev_id) {
  fv_wgpu_destroy_device(dev_id);
}

int32_t pion_wgpu_device_name(int32_t dev_id, void *out_buf, int32_t cap) {
  return fv_wgpu_device_name(dev_id, (char *)out_buf, cap);
}

int64_t pion_wgpu_device_max_buffer_length(int32_t dev_id) {
  return fv_wgpu_device_max_buffer_length(dev_id);
}

int32_t pion_wgpu_last_error(int32_t dev_id, void *out_buf, int32_t cap) {
  return fv_wgpu_last_error(dev_id, (char *)out_buf, cap);
}

int32_t pion_wgpu_create_buffer(int32_t dev_id, int32_t size, int32_t usage) {
  return fv_wgpu_create_buffer(dev_id, (int64_t)size, (uint32_t)usage);
}

void pion_wgpu_destroy_buffer(int32_t dev_id, int32_t buf_id) {
  fv_wgpu_destroy_buffer(dev_id, buf_id);
}

int32_t pion_wgpu_buffer_write(int32_t dev_id, int32_t buf_id, int32_t offset,
                               const void *data, int32_t size) {
  return fv_wgpu_buffer_write(dev_id, buf_id, (int64_t)offset, data, (int64_t)size);
}

int32_t pion_wgpu_buffer_read(int32_t dev_id, int32_t buf_id, int32_t offset,
                              void *out_data, int32_t size) {
  return fv_wgpu_buffer_read(dev_id, buf_id, (int64_t)offset, out_data, (int64_t)size);
}

int32_t pion_wgpu_create_texture_2d(int32_t dev_id, int32_t width, int32_t height,
                                    int32_t format, int32_t usage) {
  return fv_wgpu_create_texture_2d(dev_id, width, height, format, (uint32_t)usage);
}

void pion_wgpu_destroy_texture(int32_t dev_id, int32_t tex_id) {
  fv_wgpu_destroy_texture(dev_id, tex_id);
}

int32_t pion_wgpu_texture_write_2d(int32_t dev_id, int32_t tex_id,
                                   int32_t width, int32_t height,
                                   const void *rgba_data, int32_t bytes_per_row) {
  return fv_wgpu_texture_write_2d(dev_id, tex_id, width, height, rgba_data, bytes_per_row);
}

int32_t pion_wgpu_texture_read_rgba8(int32_t dev_id, int32_t tex_id,
                                     void *out_rgba, int32_t out_cap) {
  return fv_wgpu_texture_read_rgba8(dev_id, tex_id, out_rgba, out_cap);
}

int32_t pion_wgpu_create_sampler(int32_t dev_id, int32_t linear_filter, int32_t address_mode) {
  return fv_wgpu_create_sampler(dev_id, linear_filter, address_mode);
}

void pion_wgpu_destroy_sampler(int32_t dev_id, int32_t samp_id) {
  fv_wgpu_destroy_sampler(dev_id, samp_id);
}

int32_t pion_wgpu_create_shader_module_msl(int32_t dev_id, const void *msl_src, int32_t msl_len) {
  return fv_wgpu_create_shader_module_msl(dev_id, (const char *)msl_src, msl_len);
}

void pion_wgpu_destroy_shader_module(int32_t dev_id, int32_t mod_id) {
  fv_wgpu_destroy_shader_module(dev_id, mod_id);
}

int32_t pion_wgpu_create_compute_pipeline(int32_t dev_id, int32_t mod_id,
                                          const void *entry_ptr, int32_t entry_len,
                                          int32_t wg_x, int32_t wg_y, int32_t wg_z) {
  char *entry = dup_cstr(entry_ptr, entry_len);
  if (!entry) return -1;
  int32_t r = fv_wgpu_create_compute_pipeline(dev_id, mod_id, entry, wg_x, wg_y, wg_z);
  free(entry);
  return r;
}

void pion_wgpu_destroy_compute_pipeline(int32_t dev_id, int32_t pipe_id) {
  fv_wgpu_destroy_compute_pipeline(dev_id, pipe_id);
}

int32_t pion_wgpu_dispatch_compute(int32_t dev_id, int32_t pipe_id,
                                   int32_t num_bufs, const void *buf_slots, const void *buf_ids,
                                   int32_t num_texs, const void *tex_slots, const void *tex_ids,
                                   int32_t num_samps, const void *samp_slots, const void *samp_ids,
                                   int32_t gx, int32_t gy, int32_t gz) {
  return fv_wgpu_dispatch_compute(dev_id, pipe_id,
                                  num_bufs, (const int32_t *)buf_slots, (const int32_t *)buf_ids,
                                  num_texs, (const int32_t *)tex_slots, (const int32_t *)tex_ids,
                                  num_samps, (const int32_t *)samp_slots, (const int32_t *)samp_ids,
                                  gx, gy, gz);
}

int32_t pion_wgpu_create_render_pipeline(int32_t dev_id,
                                         int32_t vs_mod_id, const void *vs_ptr, int32_t vs_len,
                                         int32_t fs_mod_id, const void *fs_ptr, int32_t fs_len,
                                         int32_t color_format, int32_t blend_enable) {
  char *vs = dup_cstr(vs_ptr, vs_len);
  char *fs = dup_cstr(fs_ptr, fs_len);
  int32_t r = -1;
  if (vs && fs) {
    r = fv_wgpu_create_render_pipeline(dev_id, vs_mod_id, vs, fs_mod_id, fs,
                                       color_format, blend_enable);
  }
  free(vs);
  free(fs);
  return r;
}

void pion_wgpu_destroy_render_pipeline(int32_t dev_id, int32_t pipe_id) {
  fv_wgpu_destroy_render_pipeline(dev_id, pipe_id);
}

int32_t pion_wgpu_render_draw(int32_t dev_id, int32_t pipe_id, int32_t target_tex_id,
                              int32_t clear_enable,
                              double clear_r, double clear_g, double clear_b, double clear_a,
                              int32_t num_bufs, const void *buf_slots, const void *buf_ids,
                              int32_t num_texs, const void *tex_slots, const void *tex_ids,
                              int32_t num_samps, const void *samp_slots, const void *samp_ids,
                              int32_t primitive_type,
                              int32_t vertex_start, int32_t vertex_count, int32_t instance_count) {
  return fv_wgpu_render_draw(dev_id, pipe_id, target_tex_id,
                             clear_enable, clear_r, clear_g, clear_b, clear_a,
                             num_bufs, (const int32_t *)buf_slots, (const int32_t *)buf_ids,
                             num_texs, (const int32_t *)tex_slots, (const int32_t *)tex_ids,
                             num_samps, (const int32_t *)samp_slots, (const int32_t *)samp_ids,
                             primitive_type, vertex_start, vertex_count, instance_count);
}

void pion_wgpu_write_f32(void *buf, int32_t byte_off, double v) {
  float f = (float)v;
  memcpy((uint8_t *)buf + byte_off, &f, 4u);
}

double pion_wgpu_read_f32(const void *buf, int32_t byte_off) {
  float f = 0.0f;
  memcpy(&f, (const uint8_t *)buf + byte_off, 4u);
  return (double)f;
}

void pion_wgpu_write_i32(void *buf, int32_t byte_off, int32_t v) {
  memcpy((uint8_t *)buf + byte_off, &v, 4u);
}

int32_t pion_wgpu_read_i32(const void *buf, int32_t byte_off) {
  int32_t v = 0;
  memcpy(&v, (const uint8_t *)buf + byte_off, 4u);
  return v;
}

int32_t pion_wgpu_write_file(const void *path_ptr, int32_t path_len,
                             const void *data, int32_t data_len) {
  char *path = dup_cstr(path_ptr, path_len);
  if (!path) return -1;
  FILE *f = fopen(path, "wb");
  free(path);
  if (!f) return -2;
  int32_t ok = 0;
  if (data_len > 0 && data) {
    size_t w = fwrite(data, 1u, (size_t)data_len, f);
    if (w != (size_t)data_len) ok = -3;
  }
  fclose(f);
  return ok;
}
