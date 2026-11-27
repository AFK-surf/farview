#include "../../../native/page/webgl_gl.h"
#include <stdint.h>
#include <stdio.h>
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

void pion_webgl_shader_source(int32_t ctx_id, int32_t shader, const void *src, int32_t len) {
  fv_webgl_shader_source(ctx_id, (uint32_t)shader, (const char *)src, len);
}

int32_t pion_webgl_get_shader_info_log(int32_t ctx_id, int32_t shader, void *out_buf, int32_t max_len) {
  return fv_webgl_get_shader_info_log(ctx_id, (uint32_t)shader, (char *)out_buf, max_len);
}

int32_t pion_webgl_get_program_info_log(int32_t ctx_id, int32_t program, void *out_buf, int32_t max_len) {
  return fv_webgl_get_program_info_log(ctx_id, (uint32_t)program, (char *)out_buf, max_len);
}

int32_t pion_webgl_get_attrib_location(int32_t ctx_id, int32_t program, const void *name, int32_t len) {
  char *s = dup_cstr(name, len);
  if (!s) return -1;
  int32_t loc = fv_webgl_get_attrib_location(ctx_id, (uint32_t)program, s);
  free(s);
  return loc;
}

void pion_webgl_bind_attrib_location(int32_t ctx_id, int32_t program, int32_t index, const void *name, int32_t len) {
  char *s = dup_cstr(name, len);
  if (!s) return;
  fv_webgl_bind_attrib_location(ctx_id, (uint32_t)program, (uint32_t)index, s);
  free(s);
}

int32_t pion_webgl_get_uniform_location(int32_t ctx_id, int32_t program, const void *name, int32_t len) {
  char *s = dup_cstr(name, len);
  if (!s) return -1;
  int32_t loc = fv_webgl_get_uniform_location(ctx_id, (uint32_t)program, s);
  free(s);
  return loc;
}

int32_t pion_webgl_get_uniform_block_index(int32_t ctx_id, int32_t program, const void *name, int32_t len) {
  char *s = dup_cstr(name, len);
  if (!s) return -1;
  uint32_t idx = fv_webgl_get_uniform_block_index(ctx_id, (uint32_t)program, s);
  free(s);
  return idx == 0xFFFFFFFFu ? -1 : (int32_t)idx;
}

/* Writes "size\ttype\tname" into out_buf and returns byte length, or -1 on error */
int32_t pion_webgl_get_active_attrib(int32_t ctx_id, int32_t program, int32_t index, void *out_buf, int32_t max_len) {
  int32_t sz = 0;
  uint32_t ty = 0;
  char name[512];
  int32_t nlen = fv_webgl_get_active_attrib(ctx_id, (uint32_t)program, (uint32_t)index, &sz, &ty, name, sizeof(name));
  if (nlen <= 0) return -1;
  int32_t written = snprintf((char *)out_buf, (size_t)max_len, "%d\t%u\t%s", sz, ty, name);
  if (written < 0) return -1;
  if (written >= max_len) written = max_len - 1;
  return written;
}

int32_t pion_webgl_get_active_uniform(int32_t ctx_id, int32_t program, int32_t index, void *out_buf, int32_t max_len) {
  int32_t sz = 0;
  uint32_t ty = 0;
  char name[512];
  int32_t nlen = fv_webgl_get_active_uniform(ctx_id, (uint32_t)program, (uint32_t)index, &sz, &ty, name, sizeof(name));
  if (nlen <= 0) return -1;
  int32_t written = snprintf((char *)out_buf, (size_t)max_len, "%d\t%u\t%s", sz, ty, name);
  if (written < 0) return -1;
  if (written >= max_len) written = max_len - 1;
  return written;
}

void pion_webgl_buffer_data(int32_t ctx_id, int32_t target, int32_t size, const void *data, int32_t has_data, int32_t usage) {
  fv_webgl_buffer_data(ctx_id, (uint32_t)target, size, has_data ? data : NULL, (uint32_t)usage);
}

void pion_webgl_buffer_sub_data(int32_t ctx_id, int32_t target, int32_t offset, int32_t size, const void *data) {
  fv_webgl_buffer_sub_data(ctx_id, (uint32_t)target, offset, size, data);
}

void pion_webgl_vertex_attrib_fv(int32_t ctx_id, int32_t index, int32_t dim, const void *f32_bytes) {
  fv_webgl_vertex_attrib_fv(ctx_id, (uint32_t)index, dim, (const float *)f32_bytes);
}

void pion_webgl_uniform_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const void *f32_bytes) {
  fv_webgl_uniform_fv(ctx_id, loc, dim, count, (const float *)f32_bytes);
}

void pion_webgl_uniform_iv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const void *i32_bytes) {
  fv_webgl_uniform_iv(ctx_id, loc, dim, count, (const int32_t *)i32_bytes);
}

void pion_webgl_uniform_uiv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const void *u32_bytes) {
  fv_webgl_uniform_uiv(ctx_id, loc, dim, count, (const uint32_t *)u32_bytes);
}

void pion_webgl_uniform_matrix_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, int32_t transpose, const void *f32_bytes) {
  fv_webgl_uniform_matrix_fv(ctx_id, loc, dim, count, transpose, (const float *)f32_bytes);
}

void pion_webgl_tex_image_2d(int32_t ctx_id, int32_t target, int32_t level, int32_t internalformat,
                             int32_t width, int32_t height, int32_t border, int32_t format,
                             int32_t type, const void *data, int32_t has_data, int32_t data_len) {
  fv_webgl_tex_image_2d(ctx_id, (uint32_t)target, level, internalformat, width, height, border,
                        (uint32_t)format, (uint32_t)type, has_data ? data : NULL, data_len);
}

void pion_webgl_tex_sub_image_2d(int32_t ctx_id, int32_t target, int32_t level,
                                 int32_t xoffset, int32_t yoffset, int32_t width, int32_t height,
                                 int32_t format, int32_t type, const void *data, int32_t data_len) {
  fv_webgl_tex_sub_image_2d(ctx_id, (uint32_t)target, level, xoffset, yoffset, width, height,
                            (uint32_t)format, (uint32_t)type, data, data_len);
}

void pion_webgl_tex_parameterf(int32_t ctx_id, int32_t target, int32_t pname, double param) {
  fv_webgl_tex_parameterf(ctx_id, (uint32_t)target, (uint32_t)pname, (float)param);
}

void pion_webgl_draw_buffers(int32_t ctx_id, int32_t count, const void *u32_bytes) {
  fv_webgl_draw_buffers(ctx_id, count, (const uint32_t *)u32_bytes);
}

void pion_webgl_clear_color(int32_t ctx_id, double r, double g, double b, double a) {
  fv_webgl_clear_color(ctx_id, (float)r, (float)g, (float)b, (float)a);
}

void pion_webgl_clear_depth(int32_t ctx_id, double d) {
  fv_webgl_clear_depth(ctx_id, (float)d);
}

void pion_webgl_depth_range(int32_t ctx_id, double znear, double zfar) {
  fv_webgl_depth_range(ctx_id, (float)znear, (float)zfar);
}

void pion_webgl_blend_color(int32_t ctx_id, double r, double g, double b, double a) {
  fv_webgl_blend_color(ctx_id, (float)r, (float)g, (float)b, (float)a);
}

void pion_webgl_line_width(int32_t ctx_id, double width) {
  fv_webgl_line_width(ctx_id, (float)width);
}

void pion_webgl_polygon_offset(int32_t ctx_id, double factor, double units) {
  fv_webgl_polygon_offset(ctx_id, (float)factor, (float)units);
}

void pion_webgl_sampler_parameterf(int32_t ctx_id, int32_t sampler, int32_t pname, double param) {
  fv_webgl_sampler_parameterf(ctx_id, (uint32_t)sampler, (uint32_t)pname, (float)param);
}

void pion_webgl_clear_buffer_fv(int32_t ctx_id, int32_t buffer, int32_t drawbuffer, const void *f32_bytes) {
  fv_webgl_clear_buffer_fv(ctx_id, (uint32_t)buffer, drawbuffer, (const float *)f32_bytes);
}

void pion_webgl_clear_buffer_iv(int32_t ctx_id, int32_t buffer, int32_t drawbuffer, const void *i32_bytes) {
  fv_webgl_clear_buffer_iv(ctx_id, (uint32_t)buffer, drawbuffer, (const int32_t *)i32_bytes);
}

void pion_webgl_clear_buffer_uiv(int32_t ctx_id, int32_t buffer, int32_t drawbuffer, const void *u32_bytes) {
  fv_webgl_clear_buffer_uiv(ctx_id, (uint32_t)buffer, drawbuffer, (const uint32_t *)u32_bytes);
}

void pion_webgl_clear_buffer_fi(int32_t ctx_id, int32_t buffer, int32_t drawbuffer, double depth, int32_t stencil) {
  fv_webgl_clear_buffer_fi(ctx_id, (uint32_t)buffer, drawbuffer, (float)depth, stencil);
}

int32_t pion_webgl_get_string(int32_t ctx_id, int32_t pname, void *out_buf, int32_t max_len) {
  return fv_webgl_get_string(ctx_id, (uint32_t)pname, (char *)out_buf, max_len);
}

int32_t pion_webgl_get_integerv(int32_t ctx_id, int32_t pname, void *out_i32_bytes, int32_t max_count) {
  return fv_webgl_get_integerv(ctx_id, (uint32_t)pname, (int32_t *)out_i32_bytes, max_count);
}

int32_t pion_webgl_get_floatv(int32_t ctx_id, int32_t pname, void *out_f32_bytes, int32_t max_count) {
  return fv_webgl_get_floatv(ctx_id, (uint32_t)pname, (float *)out_f32_bytes, max_count);
}

int32_t pion_webgl_read_pixels(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t format, int32_t type, void *out_buf, int32_t buf_len) {
  return fv_webgl_read_pixels(ctx_id, x, y, w, h, (uint32_t)format, (uint32_t)type, out_buf, buf_len);
}

int32_t pion_webgl_read_rgba_top_down(int32_t ctx_id, void *out_buf, int32_t buf_len) {
  return fv_webgl_read_rgba_top_down(ctx_id, out_buf, buf_len);
}

int32_t pion_webgl_read_rgba_scaled(int32_t ctx_id, int32_t max_dim, int32_t keep_premul,
                                    void *out_buf, int32_t buf_len) {
  return fv_webgl_read_rgba_scaled(ctx_id, max_dim, keep_premul, out_buf, buf_len);
}

int64_t pion_webgl_batch_fn_ptr(void) {
  return (int64_t)(uintptr_t)&fv_webgl_exec_batch;
}

int64_t pion_webgl_upload_fn_ptr(void) {
  return (int64_t)(uintptr_t)&fv_webgl_exec_upload;
}

/* Helper to pack a double as IEEE-754 float32 into a 4-byte slot in a Bytes buffer */
void pion_webgl_write_f32(void *buf, int32_t byte_off, double val) {
  float f = (float)val;
  memcpy((uint8_t *)buf + byte_off, &f, 4);
}

double pion_webgl_read_f32(const void *buf, int32_t byte_off) {
  float f = 0.0f;
  memcpy(&f, (const uint8_t *)buf + byte_off, 4);
  return (double)f;
}

void pion_webgl_write_i32(void *buf, int32_t byte_off, int32_t val) {
  memcpy((uint8_t *)buf + byte_off, &val, 4);
}

int32_t pion_webgl_read_i32(const void *buf, int32_t byte_off) {
  int32_t v = 0;
  memcpy(&v, (const uint8_t *)buf + byte_off, 4);
  return v;
}
