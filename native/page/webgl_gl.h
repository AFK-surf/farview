#ifndef FV_WEBGL_GL_H
#define FV_WEBGL_GL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FV_WEBGL_MAX_CONTEXTS 32
#define FV_WEBGL_MAX_DIM 4096

/* Availability & Context lifecycle */
int32_t fv_webgl_available(void);
int32_t fv_webgl_create(int32_t width, int32_t height, int32_t is_webgl2);
void    fv_webgl_destroy(int32_t ctx_id);
int32_t fv_webgl_resize(int32_t ctx_id, int32_t width, int32_t height);
int32_t fv_webgl_width(int32_t ctx_id);
int32_t fv_webgl_height(int32_t ctx_id);
int32_t fv_webgl_is_dirty(int32_t ctx_id);
void    fv_webgl_clear_dirty(int32_t ctx_id);
int32_t fv_webgl_active_count(void);

/* Shaders & Programs */
uint32_t fv_webgl_create_shader(int32_t ctx_id, uint32_t type);
void     fv_webgl_shader_source(int32_t ctx_id, uint32_t shader, const char *src, int32_t len);
void     fv_webgl_compile_shader(int32_t ctx_id, uint32_t shader);
int32_t  fv_webgl_get_shader_iv(int32_t ctx_id, uint32_t shader, uint32_t pname);
int32_t  fv_webgl_get_shader_info_log(int32_t ctx_id, uint32_t shader, char *out_buf, int32_t max_len);
void     fv_webgl_delete_shader(int32_t ctx_id, uint32_t shader);

uint32_t fv_webgl_create_program(int32_t ctx_id);
void     fv_webgl_attach_shader(int32_t ctx_id, uint32_t program, uint32_t shader);
void     fv_webgl_detach_shader(int32_t ctx_id, uint32_t program, uint32_t shader);
void     fv_webgl_link_program(int32_t ctx_id, uint32_t program);
void     fv_webgl_validate_program(int32_t ctx_id, uint32_t program);
int32_t  fv_webgl_get_program_iv(int32_t ctx_id, uint32_t program, uint32_t pname);
int32_t  fv_webgl_get_program_info_log(int32_t ctx_id, uint32_t program, char *out_buf, int32_t max_len);
void     fv_webgl_use_program(int32_t ctx_id, uint32_t program);
void     fv_webgl_delete_program(int32_t ctx_id, uint32_t program);
int32_t  fv_webgl_get_attrib_location(int32_t ctx_id, uint32_t program, const char *name);
void     fv_webgl_bind_attrib_location(int32_t ctx_id, uint32_t program, uint32_t index, const char *name);
int32_t  fv_webgl_get_uniform_location(int32_t ctx_id, uint32_t program, const char *name);
uint32_t fv_webgl_get_uniform_block_index(int32_t ctx_id, uint32_t program, const char *name);
void     fv_webgl_uniform_block_binding(int32_t ctx_id, uint32_t program, uint32_t block_index, uint32_t block_binding);
void     fv_webgl_bind_buffer_base(int32_t ctx_id, uint32_t target, uint32_t index, uint32_t buffer);
void     fv_webgl_bind_buffer_range(int32_t ctx_id, uint32_t target, uint32_t index, uint32_t buffer,
                                    int32_t offset, int32_t size);
int32_t  fv_webgl_get_active_attrib(int32_t ctx_id, uint32_t program, uint32_t index,
                                    int32_t *out_size, uint32_t *out_type, char *out_name, int32_t max_len);
int32_t  fv_webgl_get_active_uniform(int32_t ctx_id, uint32_t program, uint32_t index,
                                     int32_t *out_size, uint32_t *out_type, char *out_name, int32_t max_len);

/* Buffers, VAOs & Vertex Attributes */
uint32_t fv_webgl_create_buffer(int32_t ctx_id);
void     fv_webgl_bind_buffer(int32_t ctx_id, uint32_t target, uint32_t buffer);
void     fv_webgl_buffer_data(int32_t ctx_id, uint32_t target, int32_t size, const void *data, uint32_t usage);
void     fv_webgl_buffer_sub_data(int32_t ctx_id, uint32_t target, int32_t offset, int32_t size, const void *data);
void     fv_webgl_delete_buffer(int32_t ctx_id, uint32_t buffer);

uint32_t fv_webgl_create_vao(int32_t ctx_id);
void     fv_webgl_bind_vao(int32_t ctx_id, uint32_t vao);
void     fv_webgl_delete_vao(int32_t ctx_id, uint32_t vao);

void     fv_webgl_enable_vertex_attrib_array(int32_t ctx_id, uint32_t index);
void     fv_webgl_disable_vertex_attrib_array(int32_t ctx_id, uint32_t index);
void     fv_webgl_vertex_attrib_pointer(int32_t ctx_id, uint32_t index, int32_t size,
                                        uint32_t type, int32_t normalized, int32_t stride, int32_t offset);
void     fv_webgl_vertex_attrib_ipointer(int32_t ctx_id, uint32_t index, int32_t size,
                                         uint32_t type, int32_t stride, int32_t offset);
void     fv_webgl_vertex_attrib_divisor(int32_t ctx_id, uint32_t index, uint32_t divisor);
void     fv_webgl_vertex_attrib_fv(int32_t ctx_id, uint32_t index, int32_t dim, const float *vals);

/* Uniforms */
void     fv_webgl_uniform_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const float *vals);
void     fv_webgl_uniform_iv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const int32_t *vals);
void     fv_webgl_uniform_uiv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const uint32_t *vals);
void     fv_webgl_uniform_matrix_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count,
                                    int32_t transpose, const float *vals);

/* Textures & PixelStore */
uint32_t fv_webgl_create_texture(int32_t ctx_id);
void     fv_webgl_bind_texture(int32_t ctx_id, uint32_t target, uint32_t texture);
void     fv_webgl_active_texture(int32_t ctx_id, uint32_t unit);
void     fv_webgl_delete_texture(int32_t ctx_id, uint32_t texture);
void     fv_webgl_pixel_storei(int32_t ctx_id, uint32_t pname, int32_t param);
void     fv_webgl_tex_image_2d(int32_t ctx_id, uint32_t target, int32_t level, int32_t internalformat,
                               int32_t width, int32_t height, int32_t border, uint32_t format,
                               uint32_t type, const void *data, int32_t data_len);
void     fv_webgl_tex_sub_image_2d(int32_t ctx_id, uint32_t target, int32_t level,
                                   int32_t xoffset, int32_t yoffset, int32_t width, int32_t height,
                                   uint32_t format, uint32_t type, const void *data, int32_t data_len);
void     fv_webgl_tex_parameteri(int32_t ctx_id, uint32_t target, uint32_t pname, int32_t param);
void     fv_webgl_tex_parameterf(int32_t ctx_id, uint32_t target, uint32_t pname, float param);
void     fv_webgl_generate_mipmap(int32_t ctx_id, uint32_t target);

/* Samplers (WebGL2) */
uint32_t fv_webgl_create_sampler(int32_t ctx_id);
void     fv_webgl_delete_sampler(int32_t ctx_id, uint32_t sampler);
void     fv_webgl_bind_sampler(int32_t ctx_id, uint32_t unit, uint32_t sampler);
void     fv_webgl_sampler_parameteri(int32_t ctx_id, uint32_t sampler, uint32_t pname, int32_t param);
void     fv_webgl_sampler_parameterf(int32_t ctx_id, uint32_t sampler, uint32_t pname, float param);

/* Framebuffers & Renderbuffers */
uint32_t fv_webgl_create_framebuffer(int32_t ctx_id);
void     fv_webgl_bind_framebuffer(int32_t ctx_id, uint32_t target, uint32_t fbo);
void     fv_webgl_framebuffer_texture_2d(int32_t ctx_id, uint32_t target, uint32_t attachment,
                                         uint32_t textarget, uint32_t texture, int32_t level);
void     fv_webgl_framebuffer_renderbuffer(int32_t ctx_id, uint32_t target, uint32_t attachment,
                                           uint32_t rbtarget, uint32_t rbo);
uint32_t fv_webgl_check_framebuffer_status(int32_t ctx_id, uint32_t target);
void     fv_webgl_delete_framebuffer(int32_t ctx_id, uint32_t fbo);
void     fv_webgl_draw_buffers(int32_t ctx_id, int32_t count, const uint32_t *bufs);
void     fv_webgl_read_buffer(int32_t ctx_id, uint32_t src);
void     fv_webgl_blit_framebuffer(int32_t ctx_id, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                   int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                   uint32_t mask, uint32_t filter);

uint32_t fv_webgl_create_renderbuffer(int32_t ctx_id);
void     fv_webgl_bind_renderbuffer(int32_t ctx_id, uint32_t target, uint32_t rbo);
void     fv_webgl_renderbuffer_storage(int32_t ctx_id, uint32_t target, uint32_t internalformat,
                                       int32_t width, int32_t height);
void     fv_webgl_renderbuffer_storage_multisample(int32_t ctx_id, uint32_t target, int32_t samples,
                                                   uint32_t internalformat, int32_t width, int32_t height);
void     fv_webgl_delete_renderbuffer(int32_t ctx_id, uint32_t rbo);

/* State & Draw */
void     fv_webgl_viewport(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h);
void     fv_webgl_scissor(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h);
void     fv_webgl_clear_color(int32_t ctx_id, float r, float g, float b, float a);
void     fv_webgl_clear_depth(int32_t ctx_id, float d);
void     fv_webgl_clear_stencil(int32_t ctx_id, int32_t s);
void     fv_webgl_clear(int32_t ctx_id, uint32_t mask);
void     fv_webgl_clear_buffer_fv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const float *vals);
void     fv_webgl_clear_buffer_iv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const int32_t *vals);
void     fv_webgl_clear_buffer_uiv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const uint32_t *vals);
void     fv_webgl_clear_buffer_fi(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil);
void     fv_webgl_enable(int32_t ctx_id, uint32_t cap);
void     fv_webgl_disable(int32_t ctx_id, uint32_t cap);
int32_t  fv_webgl_is_enabled(int32_t ctx_id, uint32_t cap);
void     fv_webgl_depth_func(int32_t ctx_id, uint32_t func);
void     fv_webgl_depth_mask(int32_t ctx_id, int32_t flag);
void     fv_webgl_depth_range(int32_t ctx_id, float znear, float zfar);
void     fv_webgl_color_mask(int32_t ctx_id, int32_t r, int32_t g, int32_t b, int32_t a);
void     fv_webgl_cull_face(int32_t ctx_id, uint32_t mode);
void     fv_webgl_front_face(int32_t ctx_id, uint32_t mode);
void     fv_webgl_blend_func(int32_t ctx_id, uint32_t sfactor, uint32_t dfactor);
void     fv_webgl_blend_func_separate(int32_t ctx_id, uint32_t src_rgb, uint32_t dst_rgb,
                                      uint32_t src_a, uint32_t dst_a);
void     fv_webgl_blend_equation(int32_t ctx_id, uint32_t mode);
void     fv_webgl_blend_equation_separate(int32_t ctx_id, uint32_t mode_rgb, uint32_t mode_a);
void     fv_webgl_blend_color(int32_t ctx_id, float r, float g, float b, float a);
void     fv_webgl_line_width(int32_t ctx_id, float width);
void     fv_webgl_polygon_offset(int32_t ctx_id, float factor, float units);
void     fv_webgl_stencil_func(int32_t ctx_id, uint32_t func, int32_t ref, uint32_t mask);
void     fv_webgl_stencil_func_separate(int32_t ctx_id, uint32_t face, uint32_t func, int32_t ref, uint32_t mask);
void     fv_webgl_stencil_op(int32_t ctx_id, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
void     fv_webgl_stencil_op_separate(int32_t ctx_id, uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
void     fv_webgl_stencil_mask(int32_t ctx_id, uint32_t mask);
void     fv_webgl_stencil_mask_separate(int32_t ctx_id, uint32_t face, uint32_t mask);

void     fv_webgl_draw_arrays(int32_t ctx_id, uint32_t mode, int32_t first, int32_t count);
void     fv_webgl_draw_elements(int32_t ctx_id, uint32_t mode, int32_t count, uint32_t type, int32_t offset);
void     fv_webgl_draw_arrays_instanced(int32_t ctx_id, uint32_t mode, int32_t first, int32_t count, int32_t primcount);
void     fv_webgl_draw_elements_instanced(int32_t ctx_id, uint32_t mode, int32_t count, uint32_t type,
                                          int32_t offset, int32_t primcount);
void     fv_webgl_finish(int32_t ctx_id);
void     fv_webgl_flush(int32_t ctx_id);
uint32_t fv_webgl_get_error(int32_t ctx_id);
int32_t  fv_webgl_get_string(int32_t ctx_id, uint32_t pname, char *out_buf, int32_t max_len);
int32_t  fv_webgl_get_integerv(int32_t ctx_id, uint32_t pname, int32_t *out_vals, int32_t max_count);
int32_t  fv_webgl_get_floatv(int32_t ctx_id, uint32_t pname, float *out_vals, int32_t max_count);

/* Readback */
int32_t  fv_webgl_read_pixels(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t format, uint32_t type, void *out_buf, int32_t buf_len);
int32_t  fv_webgl_read_rgba_top_down(int32_t ctx_id, void *out_buf, int32_t buf_len);
int32_t  fv_webgl_read_rgba_scaled(int32_t ctx_id, int32_t max_dim, int32_t keep_premul,
                                   void *out_buf, int32_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* FV_WEBGL_GL_H */
