// Farview Wave WebGPU-Metal-1: Real Apple Silicon Metal backend for WebGPU.
// Pure C + Objective-C Runtime on macOS (#ifdef __APPLE__), graceful stub on Linux/other.
#ifndef FARVIEW_WEBGPU_METAL_H
#define FARVIEW_WEBGPU_METAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Texture pixel formats
#define FV_WGPU_FMT_RGBA8_UNORM   0
#define FV_WGPU_FMT_BGRA8_UNORM   1
#define FV_WGPU_FMT_DEPTH32_FLOAT 2

// Primitive topologies
#define FV_WGPU_PRIM_POINT_LIST     0
#define FV_WGPU_PRIM_LINE_LIST      1
#define FV_WGPU_PRIM_LINE_STRIP     2
#define FV_WGPU_PRIM_TRIANGLE_LIST  3
#define FV_WGPU_PRIM_TRIANGLE_STRIP 4

// Sampler address modes
#define FV_WGPU_ADDR_CLAMP_TO_EDGE 0
#define FV_WGPU_ADDR_REPEAT        1
#define FV_WGPU_ADDR_MIRROR_REPEAT 2

// Texture usage flags (WebGPU-compatible bitmask)
#define FV_WGPU_TEX_USAGE_COPY_SRC          1u
#define FV_WGPU_TEX_USAGE_COPY_DST          2u
#define FV_WGPU_TEX_USAGE_TEXTURE_BINDING   4u
#define FV_WGPU_TEX_USAGE_STORAGE_BINDING   8u
#define FV_WGPU_TEX_USAGE_RENDER_ATTACHMENT 16u

// Availability & Device management
int32_t fv_wgpu_available(void);
int32_t fv_wgpu_create_device(void);
void    fv_wgpu_destroy_device(int32_t dev_id);
int32_t fv_wgpu_device_name(int32_t dev_id, char *out_buf, int32_t cap);
int64_t fv_wgpu_device_max_buffer_length(int32_t dev_id);
int32_t fv_wgpu_last_error(int32_t dev_id, char *out_buf, int32_t cap);

// Buffer management
int32_t fv_wgpu_create_buffer(int32_t dev_id, int64_t size, uint32_t usage);
void    fv_wgpu_destroy_buffer(int32_t dev_id, int32_t buf_id);
int32_t fv_wgpu_buffer_write(int32_t dev_id, int32_t buf_id, int64_t offset,
                             const void *data, int64_t size);
int32_t fv_wgpu_buffer_read(int32_t dev_id, int32_t buf_id, int64_t offset,
                            void *out_data, int64_t size);

// Texture & Sampler management
int32_t fv_wgpu_create_texture_2d(int32_t dev_id, int32_t width, int32_t height,
                                  int32_t format, uint32_t usage);
void    fv_wgpu_destroy_texture(int32_t dev_id, int32_t tex_id);
int32_t fv_wgpu_texture_write_2d(int32_t dev_id, int32_t tex_id,
                                 int32_t width, int32_t height,
                                 const void *rgba_data, int32_t bytes_per_row);
int32_t fv_wgpu_texture_read_rgba8(int32_t dev_id, int32_t tex_id,
                                   void *out_rgba, int32_t out_cap);

int32_t fv_wgpu_create_sampler(int32_t dev_id, int32_t linear_filter, int32_t address_mode);
void    fv_wgpu_destroy_sampler(int32_t dev_id, int32_t samp_id);

// ShaderModule (MSL source)
int32_t fv_wgpu_create_shader_module_msl(int32_t dev_id, const char *msl_src, int32_t msl_len);
void    fv_wgpu_destroy_shader_module(int32_t dev_id, int32_t mod_id);

// Compute Pipeline & Dispatch
int32_t fv_wgpu_create_compute_pipeline(int32_t dev_id, int32_t mod_id,
                                        const char *entry_name,
                                        int32_t wg_x, int32_t wg_y, int32_t wg_z);
void    fv_wgpu_destroy_compute_pipeline(int32_t dev_id, int32_t pipe_id);
int32_t fv_wgpu_dispatch_compute(int32_t dev_id, int32_t pipe_id,
                                 int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                                 int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                                 int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                                 int32_t gx, int32_t gy, int32_t gz);

// Render Pipeline & Draw
int32_t fv_wgpu_create_render_pipeline(int32_t dev_id,
                                       int32_t vs_mod_id, const char *vs_entry,
                                       int32_t fs_mod_id, const char *fs_entry,
                                       int32_t color_format, int32_t blend_enable);
void    fv_wgpu_destroy_render_pipeline(int32_t dev_id, int32_t pipe_id);
int32_t fv_wgpu_render_draw(int32_t dev_id, int32_t pipe_id, int32_t target_tex_id,
                            int32_t clear_enable,
                            double clear_r, double clear_g, double clear_b, double clear_a,
                            int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                            int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                            int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                            int32_t primitive_type,
                            int32_t vertex_start, int32_t vertex_count, int32_t instance_count);

#ifdef __cplusplus
}
#endif

#endif // FARVIEW_WEBGPU_METAL_H
