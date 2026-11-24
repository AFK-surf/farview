// Farview Wave WebGPU-Metal-1: Real Apple Silicon Metal backend for WebGPU.
// Uses MTLCreateSystemDefaultDevice() + Objective-C Runtime on macOS (#ifdef __APPLE__).
// Non-Apple platforms compile cleanly with graceful unavailable stubs.

#include "webgpu_metal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__

#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>

extern id MTLCreateSystemDefaultDevice(void);

typedef struct {
  unsigned long width;
  unsigned long height;
  unsigned long depth;
} FvMTLSize;

typedef struct {
  unsigned long x;
  unsigned long y;
  unsigned long z;
} FvMTLOrigin;

typedef struct {
  FvMTLOrigin origin;
  FvMTLSize size;
} FvMTLRegion;

typedef struct {
  double red;
  double green;
  double blue;
  double alpha;
} FvMTLClearColor;

// Typed objc_msgSend helpers (strict ABI casts required on arm64)
static inline id m_id0(id obj, SEL sel) {
  return ((id (*)(id, SEL))objc_msgSend)(obj, sel);
}
static inline id m_id1(id obj, SEL sel, id a1) {
  return ((id (*)(id, SEL, id))objc_msgSend)(obj, sel, a1);
}
static inline id m_id1_cstr(id obj, SEL sel, const char *a1) {
  return ((id (*)(id, SEL, const char *))objc_msgSend)(obj, sel, a1);
}
static inline id m_id1_ulong(id obj, SEL sel, unsigned long a1) {
  return ((id (*)(id, SEL, unsigned long))objc_msgSend)(obj, sel, a1);
}
static inline id m_id2_ulong(id obj, SEL sel, unsigned long a1, unsigned long a2) {
  return ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(obj, sel, a1, a2);
}
static inline id m_id2_err(id obj, SEL sel, id a1, id *err) {
  return ((id (*)(id, SEL, id, id *))objc_msgSend)(obj, sel, a1, err);
}
static inline id m_id3_err(id obj, SEL sel, id a1, id a2, id *err) {
  return ((id (*)(id, SEL, id, id, id *))objc_msgSend)(obj, sel, a1, a2, err);
}
static inline id m_tex2d_desc(id cls, SEL sel, unsigned long fmt, unsigned long w, unsigned long h, bool mipmapped) {
  return ((id (*)(id, SEL, unsigned long, unsigned long, unsigned long, bool))objc_msgSend)(cls, sel, fmt, w, h, mipmapped);
}
static inline void *m_ptr0(id obj, SEL sel) {
  return ((void *(*)(id, SEL))objc_msgSend)(obj, sel);
}
static inline unsigned long m_ulong0(id obj, SEL sel) {
  return ((unsigned long (*)(id, SEL))objc_msgSend)(obj, sel);
}
static inline const char *m_cstr0(id obj, SEL sel) {
  return ((const char *(*)(id, SEL))objc_msgSend)(obj, sel);
}
static inline void m_void0(id obj, SEL sel) {
  ((void (*)(id, SEL))objc_msgSend)(obj, sel);
}
static inline void m_void1(id obj, SEL sel, id a1) {
  ((void (*)(id, SEL, id))objc_msgSend)(obj, sel, a1);
}
static inline void m_void1_bool(id obj, SEL sel, bool a1) {
  ((void (*)(id, SEL, bool))objc_msgSend)(obj, sel, a1);
}
static inline void m_void1_ulong(id obj, SEL sel, unsigned long a1) {
  ((void (*)(id, SEL, unsigned long))objc_msgSend)(obj, sel, a1);
}
static inline void m_void1_clear(id obj, SEL sel, FvMTLClearColor c) {
  ((void (*)(id, SEL, FvMTLClearColor))objc_msgSend)(obj, sel, c);
}
static inline void m_set_buf(id enc, SEL sel, id buf, unsigned long offset, unsigned long index) {
  ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel, buf, offset, index);
}
static inline void m_set_obj_idx(id enc, SEL sel, id res, unsigned long index) {
  ((void (*)(id, SEL, id, unsigned long))objc_msgSend)(enc, sel, res, index);
}
static inline void m_dispatch_tg(id enc, SEL sel, FvMTLSize grid, FvMTLSize tg) {
  ((void (*)(id, SEL, FvMTLSize, FvMTLSize))objc_msgSend)(enc, sel, grid, tg);
}
static inline void m_draw_prim(id enc, SEL sel, unsigned long prim, unsigned long start,
                               unsigned long count, unsigned long instances) {
  ((void (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(
      enc, sel, prim, start, count, instances);
}
static inline void m_tex_replace(id tex, SEL sel, FvMTLRegion region, unsigned long level,
                                 const void *bytes, unsigned long bytes_per_row) {
  ((void (*)(id, SEL, FvMTLRegion, unsigned long, const void *, unsigned long))objc_msgSend)(
      tex, sel, region, level, bytes, bytes_per_row);
}
static inline void m_tex_get_bytes(id tex, SEL sel, void *out_bytes, unsigned long bytes_per_row,
                                   FvMTLRegion region, unsigned long level) {
  ((void (*)(id, SEL, void *, unsigned long, FvMTLRegion, unsigned long))objc_msgSend)(
      tex, sel, out_bytes, bytes_per_row, region, level);
}

static id ns_string_from_len(const char *s, int32_t len) {
  if (!s || len < 0) return nil;
  char *tmp = (char *)malloc((size_t)len + 1u);
  if (!tmp) return nil;
  if (len > 0) memcpy(tmp, s, (size_t)len);
  tmp[len] = '\0';
  id cls = (id)objc_getClass("NSString");
  id res = m_id1_cstr(cls, sel_registerName("stringWithUTF8String:"), tmp);
  free(tmp);
  return res;
}

static id ns_string_cstr(const char *s) {
  if (!s) return nil;
  id cls = (id)objc_getClass("NSString");
  return m_id1_cstr(cls, sel_registerName("stringWithUTF8String:"), s);
}

static id pool_push(void) {
  id cls = (id)objc_getClass("NSAutoreleasePool");
  if (!cls) return nil;
  return m_id0(m_id0(cls, sel_registerName("alloc")), sel_registerName("init"));
}

static void pool_pop(id pool) {
  if (pool) {
    m_void0(pool, sel_registerName("drain"));
  }
}

static void objc_release(id obj) {
  if (obj) {
    m_void0(obj, sel_registerName("release"));
  }
}

// Resource slot limits per device
#define MAX_DEVICES   16
#define MAX_BUFFERS   256
#define MAX_TEXTURES  128
#define MAX_SAMPLERS  64
#define MAX_SHADERS   128
#define MAX_CPIPES    128
#define MAX_RPIPES    128
#define ERR_CAP       1024

typedef struct {
  int used;
  id mtl_buf;
  int64_t size;
} WgpuBuffer;

typedef struct {
  int used;
  id mtl_tex;
  int32_t width;
  int32_t height;
  int32_t format;
} WgpuTexture;

typedef struct {
  int used;
  id mtl_samp;
} WgpuSampler;

typedef struct {
  int used;
  id mtl_lib;
} WgpuShader;

typedef struct {
  int used;
  id mtl_pso;
  int32_t wg_x;
  int32_t wg_y;
  int32_t wg_z;
} WgpuComputePipe;

typedef struct {
  int used;
  id mtl_pso;
  int32_t color_format;
} WgpuRenderPipe;

typedef struct {
  int used;
  id mtl_dev;
  id mtl_queue;
  char last_error[ERR_CAP];
  WgpuBuffer buffers[MAX_BUFFERS];
  WgpuTexture textures[MAX_TEXTURES];
  WgpuSampler samplers[MAX_SAMPLERS];
  WgpuShader shaders[MAX_SHADERS];
  WgpuComputePipe cpipes[MAX_CPIPES];
  WgpuRenderPipe rpipes[MAX_RPIPES];
} WgpuDevice;

static WgpuDevice g_devs[MAX_DEVICES];

static WgpuDevice *get_dev(int32_t dev_id) {
  if (dev_id <= 0 || dev_id > MAX_DEVICES) return NULL;
  WgpuDevice *d = &g_devs[dev_id - 1];
  return d->used ? d : NULL;
}

static void set_dev_error(WgpuDevice *d, const char *msg) {
  if (!d) return;
  if (!msg) {
    d->last_error[0] = '\0';
    return;
  }
  snprintf(d->last_error, ERR_CAP, "%s", msg);
}

static void set_dev_nserror(WgpuDevice *d, const char *prefix, id nserr) {
  if (!d) return;
  if (nserr) {
    id desc = m_id0(nserr, sel_registerName("localizedDescription"));
    const char *s = desc ? m_cstr0(desc, sel_registerName("UTF8String")) : NULL;
    if (s) {
      snprintf(d->last_error, ERR_CAP, "%s: %s", prefix, s);
      return;
    }
  }
  snprintf(d->last_error, ERR_CAP, "%s", prefix);
}

static unsigned long to_mtl_pixel_format(int32_t fmt) {
  switch (fmt) {
    case FV_WGPU_FMT_BGRA8_UNORM:   return 80ul;
    case FV_WGPU_FMT_DEPTH32_FLOAT: return 252ul;
    case FV_WGPU_FMT_RGBA8_UNORM:
    default:                        return 70ul;
  }
}

static unsigned long to_mtl_prim_type(int32_t prim) {
  switch (prim) {
    case FV_WGPU_PRIM_POINT_LIST:     return 0ul;
    case FV_WGPU_PRIM_LINE_LIST:      return 1ul;
    case FV_WGPU_PRIM_LINE_STRIP:     return 2ul;
    case FV_WGPU_PRIM_TRIANGLE_STRIP: return 4ul;
    case FV_WGPU_PRIM_TRIANGLE_LIST:
    default:                          return 3ul;
  }
}

static unsigned long to_mtl_addr_mode(int32_t mode) {
  switch (mode) {
    case FV_WGPU_ADDR_REPEAT:        return 2ul;
    case FV_WGPU_ADDR_MIRROR_REPEAT: return 3ul;
    case FV_WGPU_ADDR_CLAMP_TO_EDGE:
    default:                         return 0ul;
  }
}

int32_t fv_wgpu_available(void) {
  id pool = pool_push();
  id dev = MTLCreateSystemDefaultDevice();
  int32_t ok = dev ? 1 : 0;
  objc_release(dev);
  pool_pop(pool);
  return ok;
}

int32_t fv_wgpu_create_device(void) {
  id pool = pool_push();
  int slot = -1;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devs[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pool_pop(pool);
    return -1;
  }
  id dev = MTLCreateSystemDefaultDevice();
  if (!dev) {
    pool_pop(pool);
    return -2;
  }
  id queue = m_id0(dev, sel_registerName("newCommandQueue"));
  if (!queue) {
    objc_release(dev);
    pool_pop(pool);
    return -3;
  }
  WgpuDevice *d = &g_devs[slot];
  memset(d, 0, sizeof(*d));
  d->used = 1;
  d->mtl_dev = dev;
  d->mtl_queue = queue;
  pool_pop(pool);
  return slot + 1;
}

void fv_wgpu_destroy_device(int32_t dev_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d) return;
  id pool = pool_push();
  for (int i = 0; i < MAX_RPIPES; i++) {
    if (d->rpipes[i].used) objc_release(d->rpipes[i].mtl_pso);
  }
  for (int i = 0; i < MAX_CPIPES; i++) {
    if (d->cpipes[i].used) objc_release(d->cpipes[i].mtl_pso);
  }
  for (int i = 0; i < MAX_SHADERS; i++) {
    if (d->shaders[i].used) objc_release(d->shaders[i].mtl_lib);
  }
  for (int i = 0; i < MAX_SAMPLERS; i++) {
    if (d->samplers[i].used) objc_release(d->samplers[i].mtl_samp);
  }
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (d->textures[i].used) objc_release(d->textures[i].mtl_tex);
  }
  for (int i = 0; i < MAX_BUFFERS; i++) {
    if (d->buffers[i].used) objc_release(d->buffers[i].mtl_buf);
  }
  objc_release(d->mtl_queue);
  objc_release(d->mtl_dev);
  memset(d, 0, sizeof(*d));
  pool_pop(pool);
}

int32_t fv_wgpu_device_name(int32_t dev_id, char *out_buf, int32_t cap) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || !out_buf || cap <= 1) return -1;
  id pool = pool_push();
  id name = m_id0(d->mtl_dev, sel_registerName("name"));
  const char *s = name ? m_cstr0(name, sel_registerName("UTF8String")) : "";
  int n = snprintf(out_buf, (size_t)cap, "%s", s ? s : "");
  pool_pop(pool);
  if (n < 0) return -1;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

int64_t fv_wgpu_device_max_buffer_length(int32_t dev_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d) return -1;
  unsigned long v = m_ulong0(d->mtl_dev, sel_registerName("maxBufferLength"));
  return (int64_t)v;
}

int32_t fv_wgpu_last_error(int32_t dev_id, char *out_buf, int32_t cap) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || !out_buf || cap <= 1) return 0;
  int n = snprintf(out_buf, (size_t)cap, "%s", d->last_error);
  if (n < 0) return 0;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

int32_t fv_wgpu_create_buffer(int32_t dev_id, int64_t size, uint32_t usage) {
  (void)usage;
  WgpuDevice *d = get_dev(dev_id);
  if (!d || size <= 0) return -1;
  int slot = -1;
  for (int i = 0; i < MAX_BUFFERS; i++) {
    if (!d->buffers[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    set_dev_error(d, "max buffer count exceeded");
    return -2;
  }
  id pool = pool_push();
  // MTLResourceStorageModeShared = 0
  id buf = m_id2_ulong(d->mtl_dev, sel_registerName("newBufferWithLength:options:"),
                       (unsigned long)size, 0ul);
  pool_pop(pool);
  if (!buf) {
    set_dev_error(d, "MTLDevice newBufferWithLength:options: failed");
    return -3;
  }
  d->buffers[slot].used = 1;
  d->buffers[slot].mtl_buf = buf;
  d->buffers[slot].size = size;
  set_dev_error(d, NULL);
  return slot + 1;
}

void fv_wgpu_destroy_buffer(int32_t dev_id, int32_t buf_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || buf_id <= 0 || buf_id > MAX_BUFFERS) return;
  WgpuBuffer *b = &d->buffers[buf_id - 1];
  if (!b->used) return;
  objc_release(b->mtl_buf);
  memset(b, 0, sizeof(*b));
}

int32_t fv_wgpu_buffer_write(int32_t dev_id, int32_t buf_id, int64_t offset,
                             const void *data, int64_t size) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || buf_id <= 0 || buf_id > MAX_BUFFERS || !data || offset < 0 || size < 0) return -1;
  WgpuBuffer *b = &d->buffers[buf_id - 1];
  if (!b->used || offset + size > b->size) {
    set_dev_error(d, "buffer_write out of bounds");
    return -2;
  }
  void *ptr = m_ptr0(b->mtl_buf, sel_registerName("contents"));
  if (!ptr) {
    set_dev_error(d, "MTLBuffer contents returned NULL");
    return -3;
  }
  if (size > 0) {
    memcpy((uint8_t *)ptr + offset, data, (size_t)size);
  }
  return 0;
}

int32_t fv_wgpu_buffer_read(int32_t dev_id, int32_t buf_id, int64_t offset,
                            void *out_data, int64_t size) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || buf_id <= 0 || buf_id > MAX_BUFFERS || !out_data || offset < 0 || size < 0) return -1;
  WgpuBuffer *b = &d->buffers[buf_id - 1];
  if (!b->used || offset + size > b->size) {
    set_dev_error(d, "buffer_read out of bounds");
    return -2;
  }
  void *ptr = m_ptr0(b->mtl_buf, sel_registerName("contents"));
  if (!ptr) {
    set_dev_error(d, "MTLBuffer contents returned NULL");
    return -3;
  }
  if (size > 0) {
    memcpy(out_data, (const uint8_t *)ptr + offset, (size_t)size);
  }
  return 0;
}

int32_t fv_wgpu_create_texture_2d(int32_t dev_id, int32_t width, int32_t height,
                                  int32_t format, uint32_t usage) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || width <= 0 || height <= 0) return -1;
  int slot = -1;
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (!d->textures[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    set_dev_error(d, "max texture count exceeded");
    return -2;
  }
  id pool = pool_push();
  id desc_cls = (id)objc_getClass("MTLTextureDescriptor");
  unsigned long mtl_fmt = to_mtl_pixel_format(format);
  id desc = m_tex2d_desc(desc_cls,
                         sel_registerName("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                         mtl_fmt, (unsigned long)width, (unsigned long)height, false);
  if (!desc) {
    pool_pop(pool);
    set_dev_error(d, "MTLTextureDescriptor creation failed");
    return -3;
  }
  // Storage mode shared (0) so we can upload and readback on Apple Silicon unified memory
  m_void1_ulong(desc, sel_registerName("setStorageMode:"), 0ul);
  unsigned long mtl_usage = 0ul;
  if (usage & FV_WGPU_TEX_USAGE_TEXTURE_BINDING) mtl_usage |= 1ul; // ShaderRead
  if (usage & FV_WGPU_TEX_USAGE_STORAGE_BINDING) mtl_usage |= 2ul; // ShaderWrite
  if (usage & FV_WGPU_TEX_USAGE_RENDER_ATTACHMENT) mtl_usage |= 4ul; // RenderTarget
  if (mtl_usage == 0ul) mtl_usage = 1ul | 4ul;
  m_void1_ulong(desc, sel_registerName("setUsage:"), mtl_usage);

  id tex = m_id1(d->mtl_dev, sel_registerName("newTextureWithDescriptor:"), desc);
  pool_pop(pool);
  if (!tex) {
    set_dev_error(d, "MTLDevice newTextureWithDescriptor: failed");
    return -4;
  }
  d->textures[slot].used = 1;
  d->textures[slot].mtl_tex = tex;
  d->textures[slot].width = width;
  d->textures[slot].height = height;
  d->textures[slot].format = format;
  set_dev_error(d, NULL);
  return slot + 1;
}

void fv_wgpu_destroy_texture(int32_t dev_id, int32_t tex_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || tex_id <= 0 || tex_id > MAX_TEXTURES) return;
  WgpuTexture *t = &d->textures[tex_id - 1];
  if (!t->used) return;
  objc_release(t->mtl_tex);
  memset(t, 0, sizeof(*t));
}

int32_t fv_wgpu_texture_write_2d(int32_t dev_id, int32_t tex_id,
                                 int32_t width, int32_t height,
                                 const void *rgba_data, int32_t bytes_per_row) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || tex_id <= 0 || tex_id > MAX_TEXTURES || !rgba_data || width <= 0 || height <= 0) return -1;
  WgpuTexture *t = &d->textures[tex_id - 1];
  if (!t->used || width > t->width || height > t->height) {
    set_dev_error(d, "texture_write_2d out of bounds");
    return -2;
  }
  unsigned long bpr = (bytes_per_row > 0) ? (unsigned long)bytes_per_row : (unsigned long)width * 4ul;
  FvMTLRegion region = { { 0ul, 0ul, 0ul }, { (unsigned long)width, (unsigned long)height, 1ul } };
  m_tex_replace(t->mtl_tex,
                sel_registerName("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
                region, 0ul, rgba_data, bpr);
  return 0;
}

int32_t fv_wgpu_texture_read_rgba8(int32_t dev_id, int32_t tex_id,
                                   void *out_rgba, int32_t out_cap) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || tex_id <= 0 || tex_id > MAX_TEXTURES || !out_rgba) return -1;
  WgpuTexture *t = &d->textures[tex_id - 1];
  if (!t->used) return -2;
  int64_t need = (int64_t)t->width * (int64_t)t->height * 4;
  if (need <= 0 || need > (int64_t)out_cap) {
    set_dev_error(d, "texture_read_rgba8 buffer too small");
    return -3;
  }
  FvMTLRegion region = {
    { 0ul, 0ul, 0ul },
    { (unsigned long)t->width, (unsigned long)t->height, 1ul }
  };
  unsigned long bpr = (unsigned long)t->width * 4ul;
  m_tex_get_bytes(t->mtl_tex,
                  sel_registerName("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
                  out_rgba, bpr, region, 0ul);
  if (t->format == FV_WGPU_FMT_BGRA8_UNORM) {
    uint8_t *p = (uint8_t *)out_rgba;
    int64_t n_px = (int64_t)t->width * (int64_t)t->height;
    for (int64_t i = 0; i < n_px; i++) {
      uint8_t b = p[i * 4 + 0];
      p[i * 4 + 0] = p[i * 4 + 2];
      p[i * 4 + 2] = b;
    }
  }
  return (int32_t)need;
}

int32_t fv_wgpu_create_sampler(int32_t dev_id, int32_t linear_filter, int32_t address_mode) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d) return -1;
  int slot = -1;
  for (int i = 0; i < MAX_SAMPLERS; i++) {
    if (!d->samplers[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -2;
  id pool = pool_push();
  id cls = (id)objc_getClass("MTLSamplerDescriptor");
  id desc = m_id0(m_id0(cls, sel_registerName("alloc")), sel_registerName("init"));
  unsigned long filt = linear_filter ? 1ul : 0ul;
  unsigned long addr = to_mtl_addr_mode(address_mode);
  m_void1_ulong(desc, sel_registerName("setMinFilter:"), filt);
  m_void1_ulong(desc, sel_registerName("setMagFilter:"), filt);
  m_void1_ulong(desc, sel_registerName("setSAddressMode:"), addr);
  m_void1_ulong(desc, sel_registerName("setTAddressMode:"), addr);
  id samp = m_id1(d->mtl_dev, sel_registerName("newSamplerStateWithDescriptor:"), desc);
  objc_release(desc);
  pool_pop(pool);
  if (!samp) {
    set_dev_error(d, "newSamplerStateWithDescriptor: failed");
    return -3;
  }
  d->samplers[slot].used = 1;
  d->samplers[slot].mtl_samp = samp;
  return slot + 1;
}

void fv_wgpu_destroy_sampler(int32_t dev_id, int32_t samp_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || samp_id <= 0 || samp_id > MAX_SAMPLERS) return;
  WgpuSampler *s = &d->samplers[samp_id - 1];
  if (!s->used) return;
  objc_release(s->mtl_samp);
  memset(s, 0, sizeof(*s));
}

int32_t fv_wgpu_create_shader_module_msl(int32_t dev_id, const char *msl_src, int32_t msl_len) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || !msl_src || msl_len <= 0) return -1;
  int slot = -1;
  for (int i = 0; i < MAX_SHADERS; i++) {
    if (!d->shaders[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    set_dev_error(d, "max shader module count exceeded");
    return -2;
  }
  id pool = pool_push();
  id src = ns_string_from_len(msl_src, msl_len);
  id err = nil;
  id lib = m_id3_err(d->mtl_dev, sel_registerName("newLibraryWithSource:options:error:"),
                     src, nil, &err);
  if (!lib) {
    set_dev_nserror(d, "MSL compile error", err);
    pool_pop(pool);
    return -3;
  }
  pool_pop(pool);
  d->shaders[slot].used = 1;
  d->shaders[slot].mtl_lib = lib;
  set_dev_error(d, NULL);
  return slot + 1;
}

void fv_wgpu_destroy_shader_module(int32_t dev_id, int32_t mod_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || mod_id <= 0 || mod_id > MAX_SHADERS) return;
  WgpuShader *sh = &d->shaders[mod_id - 1];
  if (!sh->used) return;
  objc_release(sh->mtl_lib);
  memset(sh, 0, sizeof(*sh));
}

int32_t fv_wgpu_create_compute_pipeline(int32_t dev_id, int32_t mod_id,
                                        const char *entry_name,
                                        int32_t wg_x, int32_t wg_y, int32_t wg_z) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || mod_id <= 0 || mod_id > MAX_SHADERS || !entry_name) return -1;
  WgpuShader *sh = &d->shaders[mod_id - 1];
  if (!sh->used) return -2;
  int slot = -1;
  for (int i = 0; i < MAX_CPIPES; i++) {
    if (!d->cpipes[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -3;
  id pool = pool_push();
  id fn_name = ns_string_cstr(entry_name);
  id fn = m_id1(sh->mtl_lib, sel_registerName("newFunctionWithName:"), fn_name);
  if (!fn) {
    set_dev_error(d, "compute entry function not found in Metal library");
    pool_pop(pool);
    return -4;
  }
  id err = nil;
  id pso = m_id2_err(d->mtl_dev, sel_registerName("newComputePipelineStateWithFunction:error:"),
                     fn, &err);
  objc_release(fn);
  if (!pso) {
    set_dev_nserror(d, "compute pipeline creation failed", err);
    pool_pop(pool);
    return -5;
  }
  pool_pop(pool);
  d->cpipes[slot].used = 1;
  d->cpipes[slot].mtl_pso = pso;
  d->cpipes[slot].wg_x = wg_x > 0 ? wg_x : 1;
  d->cpipes[slot].wg_y = wg_y > 0 ? wg_y : 1;
  d->cpipes[slot].wg_z = wg_z > 0 ? wg_z : 1;
  set_dev_error(d, NULL);
  return slot + 1;
}

void fv_wgpu_destroy_compute_pipeline(int32_t dev_id, int32_t pipe_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || pipe_id <= 0 || pipe_id > MAX_CPIPES) return;
  WgpuComputePipe *p = &d->cpipes[pipe_id - 1];
  if (!p->used) return;
  objc_release(p->mtl_pso);
  memset(p, 0, sizeof(*p));
}

int32_t fv_wgpu_dispatch_compute(int32_t dev_id, int32_t pipe_id,
                                 int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                                 int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                                 int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                                 int32_t gx, int32_t gy, int32_t gz) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || pipe_id <= 0 || pipe_id > MAX_CPIPES) return -1;
  WgpuComputePipe *p = &d->cpipes[pipe_id - 1];
  if (!p->used || gx <= 0 || gy <= 0 || gz <= 0) return -2;

  id pool = pool_push();
  id cmd = m_id0(d->mtl_queue, sel_registerName("commandBuffer"));
  if (!cmd) {
    set_dev_error(d, "failed to create MTLCommandBuffer");
    pool_pop(pool);
    return -3;
  }
  id enc = m_id0(cmd, sel_registerName("computeCommandEncoder"));
  if (!enc) {
    set_dev_error(d, "failed to create MTLComputeCommandEncoder");
    pool_pop(pool);
    return -4;
  }
  m_void1(enc, sel_registerName("setComputePipelineState:"), p->mtl_pso);

  SEL sel_buf = sel_registerName("setBuffer:offset:atIndex:");
  for (int32_t i = 0; i < num_bufs; i++) {
    int32_t bid = buf_ids[i];
    if (bid <= 0 || bid > MAX_BUFFERS || !d->buffers[bid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid buffer handle in dispatch_compute");
      pool_pop(pool);
      return -5;
    }
    m_set_buf(enc, sel_buf, d->buffers[bid - 1].mtl_buf, 0ul, (unsigned long)buf_slots[i]);
  }
  SEL sel_tex = sel_registerName("setTexture:atIndex:");
  for (int32_t i = 0; i < num_texs; i++) {
    int32_t tid = tex_ids[i];
    if (tid <= 0 || tid > MAX_TEXTURES || !d->textures[tid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid texture handle in dispatch_compute");
      pool_pop(pool);
      return -6;
    }
    m_set_obj_idx(enc, sel_tex, d->textures[tid - 1].mtl_tex, (unsigned long)tex_slots[i]);
  }
  SEL sel_samp = sel_registerName("setSamplerState:atIndex:");
  for (int32_t i = 0; i < num_samps; i++) {
    int32_t sid = samp_ids[i];
    if (sid <= 0 || sid > MAX_SAMPLERS || !d->samplers[sid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid sampler handle in dispatch_compute");
      pool_pop(pool);
      return -7;
    }
    m_set_obj_idx(enc, sel_samp, d->samplers[sid - 1].mtl_samp, (unsigned long)samp_slots[i]);
  }

  FvMTLSize grid = { (unsigned long)gx, (unsigned long)gy, (unsigned long)gz };
  FvMTLSize tg = { (unsigned long)p->wg_x, (unsigned long)p->wg_y, (unsigned long)p->wg_z };
  m_dispatch_tg(enc, sel_registerName("dispatchThreadgroups:threadsPerThreadgroup:"), grid, tg);
  m_void0(enc, sel_registerName("endEncoding"));
  m_void0(cmd, sel_registerName("commit"));
  m_void0(cmd, sel_registerName("waitUntilCompleted"));

  id err = m_id0(cmd, sel_registerName("error"));
  if (err) {
    set_dev_nserror(d, "compute command buffer failed", err);
    pool_pop(pool);
    return -8;
  }
  pool_pop(pool);
  set_dev_error(d, NULL);
  return 0;
}

int32_t fv_wgpu_create_render_pipeline(int32_t dev_id,
                                       int32_t vs_mod_id, const char *vs_entry,
                                       int32_t fs_mod_id, const char *fs_entry,
                                       int32_t color_format, int32_t blend_enable) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || vs_mod_id <= 0 || vs_mod_id > MAX_SHADERS || !vs_entry) return -1;
  if (fs_mod_id <= 0 || fs_mod_id > MAX_SHADERS || !fs_entry) return -1;
  WgpuShader *vs_sh = &d->shaders[vs_mod_id - 1];
  WgpuShader *fs_sh = &d->shaders[fs_mod_id - 1];
  if (!vs_sh->used || !fs_sh->used) return -2;

  int slot = -1;
  for (int i = 0; i < MAX_RPIPES; i++) {
    if (!d->rpipes[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -3;

  id pool = pool_push();
  id vs_fn = m_id1(vs_sh->mtl_lib, sel_registerName("newFunctionWithName:"), ns_string_cstr(vs_entry));
  if (!vs_fn) {
    set_dev_error(d, "vertex entry function not found in Metal library");
    pool_pop(pool);
    return -4;
  }
  id fs_fn = m_id1(fs_sh->mtl_lib, sel_registerName("newFunctionWithName:"), ns_string_cstr(fs_entry));
  if (!fs_fn) {
    objc_release(vs_fn);
    set_dev_error(d, "fragment entry function not found in Metal library");
    pool_pop(pool);
    return -5;
  }

  id desc_cls = (id)objc_getClass("MTLRenderPipelineDescriptor");
  id desc = m_id0(m_id0(desc_cls, sel_registerName("alloc")), sel_registerName("init"));
  m_void1(desc, sel_registerName("setVertexFunction:"), vs_fn);
  m_void1(desc, sel_registerName("setFragmentFunction:"), fs_fn);

  id ca_arr = m_id0(desc, sel_registerName("colorAttachments"));
  id ca0 = m_id1_ulong(ca_arr, sel_registerName("objectAtIndexedSubscript:"), 0ul);
  m_void1_ulong(ca0, sel_registerName("setPixelFormat:"), to_mtl_pixel_format(color_format));
  if (blend_enable) {
    m_void1_bool(ca0, sel_registerName("setBlendingEnabled:"), true);
    m_void1_ulong(ca0, sel_registerName("setRgbBlendOperation:"), 0ul); // Add
    m_void1_ulong(ca0, sel_registerName("setAlphaBlendOperation:"), 0ul);
    m_void1_ulong(ca0, sel_registerName("setSourceRGBBlendFactor:"), 4ul); // SourceAlpha
    m_void1_ulong(ca0, sel_registerName("setDestinationRGBBlendFactor:"), 5ul); // OneMinusSourceAlpha
    m_void1_ulong(ca0, sel_registerName("setSourceAlphaBlendFactor:"), 1ul); // One
    m_void1_ulong(ca0, sel_registerName("setDestinationAlphaBlendFactor:"), 5ul);
  }

  id err = nil;
  id pso = m_id2_err(d->mtl_dev, sel_registerName("newRenderPipelineStateWithDescriptor:error:"),
                     desc, &err);
  objc_release(desc);
  objc_release(vs_fn);
  objc_release(fs_fn);

  if (!pso) {
    set_dev_nserror(d, "render pipeline creation failed", err);
    pool_pop(pool);
    return -6;
  }
  pool_pop(pool);
  d->rpipes[slot].used = 1;
  d->rpipes[slot].mtl_pso = pso;
  d->rpipes[slot].color_format = color_format;
  set_dev_error(d, NULL);
  return slot + 1;
}

void fv_wgpu_destroy_render_pipeline(int32_t dev_id, int32_t pipe_id) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || pipe_id <= 0 || pipe_id > MAX_RPIPES) return;
  WgpuRenderPipe *p = &d->rpipes[pipe_id - 1];
  if (!p->used) return;
  objc_release(p->mtl_pso);
  memset(p, 0, sizeof(*p));
}

int32_t fv_wgpu_render_draw(int32_t dev_id, int32_t pipe_id, int32_t target_tex_id,
                            int32_t clear_enable,
                            double clear_r, double clear_g, double clear_b, double clear_a,
                            int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                            int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                            int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                            int32_t primitive_type,
                            int32_t vertex_start, int32_t vertex_count, int32_t instance_count) {
  WgpuDevice *d = get_dev(dev_id);
  if (!d || pipe_id <= 0 || pipe_id > MAX_RPIPES || target_tex_id <= 0 || target_tex_id > MAX_TEXTURES) {
    return -1;
  }
  WgpuRenderPipe *p = &d->rpipes[pipe_id - 1];
  WgpuTexture *target = &d->textures[target_tex_id - 1];
  if (!p->used || !target->used || vertex_count <= 0) return -2;

  id pool = pool_push();
  id rp_cls = (id)objc_getClass("MTLRenderPassDescriptor");
  id rpd = m_id0(rp_cls, sel_registerName("renderPassDescriptor"));
  id ca_arr = m_id0(rpd, sel_registerName("colorAttachments"));
  id ca0 = m_id1_ulong(ca_arr, sel_registerName("objectAtIndexedSubscript:"), 0ul);
  m_void1(ca0, sel_registerName("setTexture:"), target->mtl_tex);
  m_void1_ulong(ca0, sel_registerName("setLoadAction:"), clear_enable ? 2ul : 1ul);
  if (clear_enable) {
    FvMTLClearColor cc = { clear_r, clear_g, clear_b, clear_a };
    m_void1_clear(ca0, sel_registerName("setClearColor:"), cc);
  }
  m_void1_ulong(ca0, sel_registerName("setStoreAction:"), 1ul); // Store

  id cmd = m_id0(d->mtl_queue, sel_registerName("commandBuffer"));
  if (!cmd) {
    set_dev_error(d, "failed to create MTLCommandBuffer");
    pool_pop(pool);
    return -3;
  }
  id enc = m_id1(cmd, sel_registerName("renderCommandEncoderWithDescriptor:"), rpd);
  if (!enc) {
    set_dev_error(d, "failed to create MTLRenderCommandEncoder");
    pool_pop(pool);
    return -4;
  }
  m_void1(enc, sel_registerName("setRenderPipelineState:"), p->mtl_pso);

  SEL sel_vbuf = sel_registerName("setVertexBuffer:offset:atIndex:");
  SEL sel_fbuf = sel_registerName("setFragmentBuffer:offset:atIndex:");
  for (int32_t i = 0; i < num_bufs; i++) {
    int32_t bid = buf_ids[i];
    if (bid <= 0 || bid > MAX_BUFFERS || !d->buffers[bid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid buffer handle in render_draw");
      pool_pop(pool);
      return -5;
    }
    id b = d->buffers[bid - 1].mtl_buf;
    unsigned long slot = (unsigned long)buf_slots[i];
    m_set_buf(enc, sel_vbuf, b, 0ul, slot);
    m_set_buf(enc, sel_fbuf, b, 0ul, slot);
  }

  SEL sel_vtex = sel_registerName("setVertexTexture:atIndex:");
  SEL sel_ftex = sel_registerName("setFragmentTexture:atIndex:");
  for (int32_t i = 0; i < num_texs; i++) {
    int32_t tid = tex_ids[i];
    if (tid <= 0 || tid > MAX_TEXTURES || !d->textures[tid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid texture handle in render_draw");
      pool_pop(pool);
      return -6;
    }
    id t = d->textures[tid - 1].mtl_tex;
    unsigned long slot = (unsigned long)tex_slots[i];
    m_set_obj_idx(enc, sel_vtex, t, slot);
    m_set_obj_idx(enc, sel_ftex, t, slot);
  }

  SEL sel_vsamp = sel_registerName("setVertexSamplerState:atIndex:");
  SEL sel_fsamp = sel_registerName("setFragmentSamplerState:atIndex:");
  for (int32_t i = 0; i < num_samps; i++) {
    int32_t sid = samp_ids[i];
    if (sid <= 0 || sid > MAX_SAMPLERS || !d->samplers[sid - 1].used) {
      m_void0(enc, sel_registerName("endEncoding"));
      set_dev_error(d, "invalid sampler handle in render_draw");
      pool_pop(pool);
      return -7;
    }
    id s = d->samplers[sid - 1].mtl_samp;
    unsigned long slot = (unsigned long)samp_slots[i];
    m_set_obj_idx(enc, sel_vsamp, s, slot);
    m_set_obj_idx(enc, sel_fsamp, s, slot);
  }

  unsigned long inst = instance_count > 0 ? (unsigned long)instance_count : 1ul;
  m_draw_prim(enc, sel_registerName("drawPrimitives:vertexStart:vertexCount:instanceCount:"),
              to_mtl_prim_type(primitive_type),
              (unsigned long)(vertex_start > 0 ? vertex_start : 0),
              (unsigned long)vertex_count,
              inst);
  m_void0(enc, sel_registerName("endEncoding"));
  m_void0(cmd, sel_registerName("commit"));
  m_void0(cmd, sel_registerName("waitUntilCompleted"));

  id err = m_id0(cmd, sel_registerName("error"));
  if (err) {
    set_dev_nserror(d, "render command buffer failed", err);
    pool_pop(pool);
    return -8;
  }
  pool_pop(pool);
  set_dev_error(d, NULL);
  return 0;
}

#else /* Non-Apple stub */

int32_t fv_wgpu_available(void) { return 0; }
int32_t fv_wgpu_create_device(void) { return -1; }
void fv_wgpu_destroy_device(int32_t dev_id) { (void)dev_id; }

int32_t fv_wgpu_device_name(int32_t dev_id, char *out_buf, int32_t cap) {
  (void)dev_id;
  if (out_buf && cap > 0) out_buf[0] = '\0';
  return -1;
}

int64_t fv_wgpu_device_max_buffer_length(int32_t dev_id) {
  (void)dev_id;
  return -1;
}

int32_t fv_wgpu_last_error(int32_t dev_id, char *out_buf, int32_t cap) {
  (void)dev_id;
  if (!out_buf || cap <= 1) return 0;
  int n = snprintf(out_buf, (size_t)cap, "WebGPU Metal backend is only available on Apple platforms");
  if (n < 0) return 0;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

int32_t fv_wgpu_create_buffer(int32_t dev_id, int64_t size, uint32_t usage) {
  (void)dev_id; (void)size; (void)usage;
  return -1;
}
void fv_wgpu_destroy_buffer(int32_t dev_id, int32_t buf_id) { (void)dev_id; (void)buf_id; }
int32_t fv_wgpu_buffer_write(int32_t dev_id, int32_t buf_id, int64_t offset,
                             const void *data, int64_t size) {
  (void)dev_id; (void)buf_id; (void)offset; (void)data; (void)size;
  return -1;
}
int32_t fv_wgpu_buffer_read(int32_t dev_id, int32_t buf_id, int64_t offset,
                            void *out_data, int64_t size) {
  (void)dev_id; (void)buf_id; (void)offset; (void)out_data; (void)size;
  return -1;
}

int32_t fv_wgpu_create_texture_2d(int32_t dev_id, int32_t width, int32_t height,
                                  int32_t format, uint32_t usage) {
  (void)dev_id; (void)width; (void)height; (void)format; (void)usage;
  return -1;
}
void fv_wgpu_destroy_texture(int32_t dev_id, int32_t tex_id) { (void)dev_id; (void)tex_id; }
int32_t fv_wgpu_texture_write_2d(int32_t dev_id, int32_t tex_id,
                                 int32_t width, int32_t height,
                                 const void *rgba_data, int32_t bytes_per_row) {
  (void)dev_id; (void)tex_id; (void)width; (void)height; (void)rgba_data; (void)bytes_per_row;
  return -1;
}
int32_t fv_wgpu_texture_read_rgba8(int32_t dev_id, int32_t tex_id,
                                   void *out_rgba, int32_t out_cap) {
  (void)dev_id; (void)tex_id; (void)out_rgba; (void)out_cap;
  return -1;
}

int32_t fv_wgpu_create_sampler(int32_t dev_id, int32_t linear_filter, int32_t address_mode) {
  (void)dev_id; (void)linear_filter; (void)address_mode;
  return -1;
}
void fv_wgpu_destroy_sampler(int32_t dev_id, int32_t samp_id) { (void)dev_id; (void)samp_id; }

int32_t fv_wgpu_create_shader_module_msl(int32_t dev_id, const char *msl_src, int32_t msl_len) {
  (void)dev_id; (void)msl_src; (void)msl_len;
  return -1;
}
void fv_wgpu_destroy_shader_module(int32_t dev_id, int32_t mod_id) { (void)dev_id; (void)mod_id; }

int32_t fv_wgpu_create_compute_pipeline(int32_t dev_id, int32_t mod_id,
                                        const char *entry_name,
                                        int32_t wg_x, int32_t wg_y, int32_t wg_z) {
  (void)dev_id; (void)mod_id; (void)entry_name; (void)wg_x; (void)wg_y; (void)wg_z;
  return -1;
}
void fv_wgpu_destroy_compute_pipeline(int32_t dev_id, int32_t pipe_id) { (void)dev_id; (void)pipe_id; }
int32_t fv_wgpu_dispatch_compute(int32_t dev_id, int32_t pipe_id,
                                 int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                                 int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                                 int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                                 int32_t gx, int32_t gy, int32_t gz) {
  (void)dev_id; (void)pipe_id;
  (void)num_bufs; (void)buf_slots; (void)buf_ids;
  (void)num_texs; (void)tex_slots; (void)tex_ids;
  (void)num_samps; (void)samp_slots; (void)samp_ids;
  (void)gx; (void)gy; (void)gz;
  return -1;
}

int32_t fv_wgpu_create_render_pipeline(int32_t dev_id,
                                       int32_t vs_mod_id, const char *vs_entry,
                                       int32_t fs_mod_id, const char *fs_entry,
                                       int32_t color_format, int32_t blend_enable) {
  (void)dev_id; (void)vs_mod_id; (void)vs_entry; (void)fs_mod_id; (void)fs_entry;
  (void)color_format; (void)blend_enable;
  return -1;
}
void fv_wgpu_destroy_render_pipeline(int32_t dev_id, int32_t pipe_id) { (void)dev_id; (void)pipe_id; }
int32_t fv_wgpu_render_draw(int32_t dev_id, int32_t pipe_id, int32_t target_tex_id,
                            int32_t clear_enable,
                            double clear_r, double clear_g, double clear_b, double clear_a,
                            int32_t num_bufs, const int32_t *buf_slots, const int32_t *buf_ids,
                            int32_t num_texs, const int32_t *tex_slots, const int32_t *tex_ids,
                            int32_t num_samps, const int32_t *samp_slots, const int32_t *samp_ids,
                            int32_t primitive_type,
                            int32_t vertex_start, int32_t vertex_count, int32_t instance_count) {
  (void)dev_id; (void)pipe_id; (void)target_tex_id;
  (void)clear_enable; (void)clear_r; (void)clear_g; (void)clear_b; (void)clear_a;
  (void)num_bufs; (void)buf_slots; (void)buf_ids;
  (void)num_texs; (void)tex_slots; (void)tex_ids;
  (void)num_samps; (void)samp_slots; (void)samp_ids;
  (void)primitive_type; (void)vertex_start; (void)vertex_count; (void)instance_count;
  return -1;
}

#endif /* __APPLE__ */
