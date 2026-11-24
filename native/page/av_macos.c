// Farview native macOS AVFoundation / CoreAudio / CoreGraphics capture & device enumeration.
// Pure C + dlopen + Objective-C Runtime: zero .m sources, zero hard-linked frameworks,
// zero compile errors on Linux.
#include "av_macos.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fv_cap_mode_name(int32_t mode) {
  switch (mode) {
    case FV_CAP_MODE_AVFOUNDATION: return "avfoundation";
    case FV_CAP_MODE_COREGRAPHICS: return "coregraphics";
    default: return "fallback";
  }
}

const char *fv_cap_reason_name(int32_t reason) {
  switch (reason) {
    case FV_CAP_REASON_OK:                 return "ok";
    case FV_CAP_REASON_UNSUPPORTED_OS:     return "unsupported_os";
    case FV_CAP_REASON_FRAMEWORK_MISSING:  return "framework_missing";
    case FV_CAP_REASON_NO_DEVICE:          return "no_hardware_device";
    case FV_CAP_REASON_PERMISSION_DENIED:  return "permission_denied";
    case FV_CAP_REASON_TCC_NOT_DETERMINED: return "tcc_not_determined";
    case FV_CAP_REASON_SESSION_ERROR:      return "session_error";
    case FV_CAP_REASON_FORCED_FALLBACK:    return "forced_fallback";
    default:                               return "unknown";
  }
}

#ifdef __APPLE__

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Minimal ABI-stable C typedefs for Objective-C, CoreFoundation, CoreAudio,
 * CoreVideo, CoreMedia, CoreGraphics, and GCD (loaded via dlopen/dlsym).
 * -------------------------------------------------------------------------- */

typedef void *FvId;
typedef void *FvClass;
typedef void *FvSEL;
typedef void *FvIvar;
typedef void *FvCFStringRef;
typedef void *FvCGImageRef;
typedef void *FvCGColorSpaceRef;
typedef void *FvCGContextRef;
typedef void *FvDispatchQueue;
typedef uint32_t FvCGDirectDisplayID;

typedef struct {
  double x;
  double y;
} FvCGPoint;

typedef struct {
  double width;
  double height;
} FvCGSize;

typedef struct {
  FvCGPoint origin;
  FvCGSize size;
} FvCGRect;

typedef struct {
  uint32_t mSelector;
  uint32_t mScope;
  uint32_t mElement;
} FvAudioObjectPropertyAddress;

#define FV_K_CF_STRING_ENCODING_UTF8 0x08000100u
#define FV_K_AUDIO_OBJECT_SYSTEM     1u
#define FV_K_AUDIO_PROP_DEVICES      0x64657623u /* 'dev#' */
#define FV_K_AUDIO_PROP_STREAMS      0x73746D23u /* 'stm#' */
#define FV_K_AUDIO_PROP_NAME         0x6C6E616Du /* 'lnam' */
#define FV_K_AUDIO_PROP_UID          0x75696420u /* 'uid ' */
#define FV_K_AUDIO_SCOPE_GLOBAL      0x676C6F62u /* 'glob' */
#define FV_K_AUDIO_SCOPE_OUTPUT      0x6F757470u /* 'outp' */

#define FV_K_CV_PIXFMT_32BGRA        0x42475241u /* 'BGRA' */
#define FV_K_CG_ALPHA_PREMULT_LAST   1u
#define FV_K_CG_BYTEORDER_32BIG      (4u << 12)

typedef struct {
  int initialized;
  void *h_objc;
  void *h_cf;
  void *h_found;
  void *h_avf;
  void *h_ca;
  void *h_cg;
  void *h_cv;
  void *h_cm;
  void *h_sck;

  /* libobjc */
  FvClass (*objc_getClass)(const char *name);
  FvSEL (*sel_registerName)(const char *str);
  void *objc_msgSend_ptr;
  FvClass (*objc_allocateClassPair)(FvClass superclass, const char *name, size_t extraBytes);
  bool (*class_addMethod)(FvClass cls, FvSEL name, void *imp, const char *types);
  bool (*class_addIvar)(FvClass cls, const char *name, size_t size, uint8_t alignment, const char *types);
  void (*objc_registerClassPair)(FvClass cls);
  FvIvar (*class_getInstanceVariable)(FvClass cls, const char *name);
  ptrdiff_t (*ivar_getOffset)(FvIvar v);

  /* CoreFoundation */
  bool (*CFStringGetCString)(FvCFStringRef str, char *buf, long bufSize, uint32_t encoding);
  void (*CFRelease)(void *cf);

  /* CoreAudio */
  int32_t (*AudioObjectGetPropertyDataSize)(uint32_t inObjectID,
                                            const FvAudioObjectPropertyAddress *inAddress,
                                            uint32_t inQualifierDataSize,
                                            const void *inQualifierData,
                                            uint32_t *outDataSize);
  int32_t (*AudioObjectGetPropertyData)(uint32_t inObjectID,
                                        const FvAudioObjectPropertyAddress *inAddress,
                                        uint32_t inQualifierDataSize,
                                        const void *inQualifierData,
                                        uint32_t *ioDataSize,
                                        void *outData);

  /* CoreGraphics */
  bool (*CGPreflightScreenCaptureAccess)(void);
  FvCGDirectDisplayID (*CGMainDisplayID)(void);
  FvCGImageRef (*CGDisplayCreateImage)(FvCGDirectDisplayID displayID);
  size_t (*CGImageGetWidth)(FvCGImageRef image);
  size_t (*CGImageGetHeight)(FvCGImageRef image);
  void (*CGImageRelease)(FvCGImageRef image);
  FvCGColorSpaceRef (*CGColorSpaceCreateDeviceRGB)(void);
  void (*CGColorSpaceRelease)(FvCGColorSpaceRef space);
  FvCGContextRef (*CGBitmapContextCreate)(void *data, size_t width, size_t height,
                                          size_t bitsPerComponent, size_t bytesPerRow,
                                          FvCGColorSpaceRef space, uint32_t bitmapInfo);
  void (*CGContextDrawImage)(FvCGContextRef c, FvCGRect rect, FvCGImageRef image);
  void (*CGContextRelease)(FvCGContextRef c);

  /* CoreMedia / CoreVideo */
  void *(*CMSampleBufferGetImageBuffer)(void *sbuf);
  void *(*CMSampleBufferGetDataBuffer)(void *sbuf);
  int32_t (*CMBlockBufferGetDataPointer)(void *theBuffer, size_t offset,
                                         size_t *lengthAtOffsetOut, size_t *totalLengthOut,
                                         char **dataPointerOut);
  int32_t (*CVPixelBufferLockBaseAddress)(void *pixelBuffer, uint64_t lockFlags);
  int32_t (*CVPixelBufferUnlockBaseAddress)(void *pixelBuffer, uint64_t unlockFlags);
  void *(*CVPixelBufferGetBaseAddress)(void *pixelBuffer);
  size_t (*CVPixelBufferGetWidth)(void *pixelBuffer);
  size_t (*CVPixelBufferGetHeight)(void *pixelBuffer);
  size_t (*CVPixelBufferGetBytesPerRow)(void *pixelBuffer);

  /* GCD (libSystem) */
  FvDispatchQueue (*dispatch_queue_create)(const char *label, void *attr);
  void (*dispatch_sync_f)(FvDispatchQueue queue, void *context, void (*work)(void *));
  void (*dispatch_release)(void *object);

  /* Delegate class registered once */
  FvClass delegate_cls;
  ptrdiff_t delegate_ctx_off;
} FvMacAvLib;

static FvMacAvLib g_mac;
static pthread_once_t g_mac_once = PTHREAD_ONCE_INIT;

/* Typed objc_msgSend wrappers (mandatory on arm64 to preserve ABI) */
static inline FvId msg0(FvId obj, FvSEL sel) {
  return ((FvId (*)(FvId, FvSEL))g_mac.objc_msgSend_ptr)(obj, sel);
}
static inline FvId msg1(FvId obj, FvSEL sel, FvId a1) {
  return ((FvId (*)(FvId, FvSEL, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline FvId msg1_cstr(FvId obj, FvSEL sel, const char *a1) {
  return ((FvId (*)(FvId, FvSEL, const char *))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline FvId msg1_u32(FvId obj, FvSEL sel, uint32_t a1) {
  return ((FvId (*)(FvId, FvSEL, uint32_t))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline FvId msg1_ulong(FvId obj, FvSEL sel, unsigned long a1) {
  return ((FvId (*)(FvId, FvSEL, unsigned long))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline FvId msg2(FvId obj, FvSEL sel, FvId a1, FvId a2) {
  return ((FvId (*)(FvId, FvSEL, FvId, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1, a2);
}
static inline FvId msg2_err(FvId obj, FvSEL sel, FvId a1, FvId *err) {
  return ((FvId (*)(FvId, FvSEL, FvId, FvId *))g_mac.objc_msgSend_ptr)(obj, sel, a1, err);
}
static inline FvId msg3_disc(FvId obj, FvSEL sel, FvId types, FvId mtype, long pos) {
  return ((FvId (*)(FvId, FvSEL, FvId, FvId, long))g_mac.objc_msgSend_ptr)(obj, sel, types, mtype, pos);
}
static inline unsigned long msg0_ulong(FvId obj, FvSEL sel) {
  return ((unsigned long (*)(FvId, FvSEL))g_mac.objc_msgSend_ptr)(obj, sel);
}
static inline long msg1_long(FvId obj, FvSEL sel, FvId a1) {
  return ((long (*)(FvId, FvSEL, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline const char *msg0_cstr(FvId obj, FvSEL sel) {
  return ((const char *(*)(FvId, FvSEL))g_mac.objc_msgSend_ptr)(obj, sel);
}
static inline bool msg1_bool(FvId obj, FvSEL sel, FvId a1) {
  return ((bool (*)(FvId, FvSEL, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline void msg0_void(FvId obj, FvSEL sel) {
  ((void (*)(FvId, FvSEL))g_mac.objc_msgSend_ptr)(obj, sel);
}
static inline void msg1_void(FvId obj, FvSEL sel, FvId a1) {
  ((void (*)(FvId, FvSEL, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline void msg1_bool_void(FvId obj, FvSEL sel, bool a1) {
  ((void (*)(FvId, FvSEL, bool))g_mac.objc_msgSend_ptr)(obj, sel, a1);
}
static inline void msg2_void(FvId obj, FvSEL sel, FvId a1, FvId a2) {
  ((void (*)(FvId, FvSEL, FvId, FvId))g_mac.objc_msgSend_ptr)(obj, sel, a1, a2);
}

static FvId ns_str(const char *cstr) {
  if (!cstr || !g_mac.objc_getClass) return NULL;
  FvClass cls = g_mac.objc_getClass("NSString");
  if (!cls) return NULL;
  return msg1_cstr((FvId)cls, g_mac.sel_registerName("stringWithUTF8String:"), cstr);
}

static void ns_to_cstr(FvId ns, char *dst, size_t cap) {
  if (!dst || cap == 0) return;
  dst[0] = '\0';
  if (!ns) return;
  const char *s = msg0_cstr(ns, g_mac.sel_registerName("UTF8String"));
  if (s) {
    snprintf(dst, cap, "%s", s);
    /* Sanitize tabs/newlines so TSV device list stays well-formed */
    for (char *p = dst; *p; p++) {
      if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
  }
}

/* Forward declaration of capture struct for delegate callback */
struct FvMacCapture {
  int32_t kind;
  int32_t mode;
  int32_t width;
  int32_t height;
  pthread_mutex_t mu;
  uint8_t *frame_buf;
  int32_t frame_cap;
  int32_t frame_len;
  uint64_t frame_seq;

  /* AVFoundation session objects (retained) */
  FvId session;
  FvId delegate;
  FvDispatchQueue queue;
};

static void delegate_did_output_sample_buffer(FvId self, FvSEL _cmd, FvId output,
                                              void *sampleBuffer, FvId connection) {
  (void)_cmd;
  (void)output;
  (void)connection;
  if (!self || !sampleBuffer || g_mac.delegate_ctx_off <= 0) return;

  FvMacCapture *cap = NULL;
  memcpy(&cap, (const uint8_t *)self + g_mac.delegate_ctx_off, sizeof(cap));
  if (!cap) return;

  if (cap->kind == 1 /* FV_CAPTURE_VIDEO */) {
    if (!g_mac.CMSampleBufferGetImageBuffer || !g_mac.CVPixelBufferLockBaseAddress) return;
    void *pb = g_mac.CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pb) return;
    if (g_mac.CVPixelBufferLockBaseAddress(pb, 1 /* kCVPixelBufferLock_ReadOnly */) != 0) return;

    const uint8_t *base = (const uint8_t *)g_mac.CVPixelBufferGetBaseAddress(pb);
    size_t sw = g_mac.CVPixelBufferGetWidth(pb);
    size_t sh = g_mac.CVPixelBufferGetHeight(pb);
    size_t stride = g_mac.CVPixelBufferGetBytesPerRow(pb);

    if (base && sw > 0 && sh > 0 && stride >= sw * 4) {
      pthread_mutex_lock(&cap->mu);
      int32_t dw = cap->width;
      int32_t dh = cap->height;
      int32_t need = dw * dh * 4;
      if (cap->frame_buf && cap->frame_cap >= need) {
        for (int32_t y = 0; y < dh; y++) {
          size_t sy = ((size_t)y * sh) / (size_t)dh;
          const uint8_t *srow = base + sy * stride;
          uint8_t *drow = cap->frame_buf + (size_t)y * (size_t)dw * 4;
          for (int32_t x = 0; x < dw; x++) {
            size_t sx = ((size_t)x * sw) / (size_t)dw;
            const uint8_t *sp = srow + sx * 4;
            /* Convert BGRA -> RGBA */
            drow[x * 4 + 0] = sp[2];
            drow[x * 4 + 1] = sp[1];
            drow[x * 4 + 2] = sp[0];
            drow[x * 4 + 3] = 255;
          }
        }
        cap->frame_len = need;
        cap->frame_seq++;
      }
      pthread_mutex_unlock(&cap->mu);
    }
    g_mac.CVPixelBufferUnlockBaseAddress(pb, 1);
  } else if (cap->kind == 0 /* FV_CAPTURE_AUDIO */) {
    if (!g_mac.CMSampleBufferGetDataBuffer || !g_mac.CMBlockBufferGetDataPointer) return;
    void *bb = g_mac.CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!bb) return;
    size_t len_at = 0, total_len = 0;
    char *ptr = NULL;
    if (g_mac.CMBlockBufferGetDataPointer(bb, 0, &len_at, &total_len, &ptr) == 0 && ptr && len_at > 0) {
      pthread_mutex_lock(&cap->mu);
      if (cap->frame_buf && cap->frame_cap > 0) {
        int32_t copy_n = (int32_t)len_at;
        if (copy_n > cap->frame_cap) copy_n = cap->frame_cap;
        memcpy(cap->frame_buf, ptr, (size_t)copy_n);
        cap->frame_len = copy_n;
        cap->frame_seq++;
      }
      pthread_mutex_unlock(&cap->mu);
    }
  }
}

static void init_mac_av(void) {
  g_mac.initialized = 1;
  g_mac.h_objc  = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_cf    = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_found = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_avf   = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_ca    = dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_cg    = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_cv    = dlopen("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_cm    = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_LAZY | RTLD_LOCAL);
  g_mac.h_sck   = dlopen("/System/Library/Frameworks/ScreenCaptureKit.framework/ScreenCaptureKit", RTLD_LAZY | RTLD_LOCAL);

  if (g_mac.h_objc) {
    *(void **)(&g_mac.objc_getClass) = dlsym(g_mac.h_objc, "objc_getClass");
    *(void **)(&g_mac.sel_registerName) = dlsym(g_mac.h_objc, "sel_registerName");
    g_mac.objc_msgSend_ptr = dlsym(g_mac.h_objc, "objc_msgSend");
    *(void **)(&g_mac.objc_allocateClassPair) = dlsym(g_mac.h_objc, "objc_allocateClassPair");
    *(void **)(&g_mac.class_addMethod) = dlsym(g_mac.h_objc, "class_addMethod");
    *(void **)(&g_mac.class_addIvar) = dlsym(g_mac.h_objc, "class_addIvar");
    *(void **)(&g_mac.objc_registerClassPair) = dlsym(g_mac.h_objc, "objc_registerClassPair");
    *(void **)(&g_mac.class_getInstanceVariable) = dlsym(g_mac.h_objc, "class_getInstanceVariable");
    *(void **)(&g_mac.ivar_getOffset) = dlsym(g_mac.h_objc, "ivar_getOffset");
  }
  if (g_mac.h_cf) {
    *(void **)(&g_mac.CFStringGetCString) = dlsym(g_mac.h_cf, "CFStringGetCString");
    *(void **)(&g_mac.CFRelease) = dlsym(g_mac.h_cf, "CFRelease");
  }
  if (g_mac.h_ca) {
    *(void **)(&g_mac.AudioObjectGetPropertyDataSize) = dlsym(g_mac.h_ca, "AudioObjectGetPropertyDataSize");
    *(void **)(&g_mac.AudioObjectGetPropertyData) = dlsym(g_mac.h_ca, "AudioObjectGetPropertyData");
  }
  if (g_mac.h_cg) {
    *(void **)(&g_mac.CGPreflightScreenCaptureAccess) = dlsym(g_mac.h_cg, "CGPreflightScreenCaptureAccess");
    *(void **)(&g_mac.CGMainDisplayID) = dlsym(g_mac.h_cg, "CGMainDisplayID");
    *(void **)(&g_mac.CGDisplayCreateImage) = dlsym(g_mac.h_cg, "CGDisplayCreateImage");
    *(void **)(&g_mac.CGImageGetWidth) = dlsym(g_mac.h_cg, "CGImageGetWidth");
    *(void **)(&g_mac.CGImageGetHeight) = dlsym(g_mac.h_cg, "CGImageGetHeight");
    *(void **)(&g_mac.CGImageRelease) = dlsym(g_mac.h_cg, "CGImageRelease");
    *(void **)(&g_mac.CGColorSpaceCreateDeviceRGB) = dlsym(g_mac.h_cg, "CGColorSpaceCreateDeviceRGB");
    *(void **)(&g_mac.CGColorSpaceRelease) = dlsym(g_mac.h_cg, "CGColorSpaceRelease");
    *(void **)(&g_mac.CGBitmapContextCreate) = dlsym(g_mac.h_cg, "CGBitmapContextCreate");
    *(void **)(&g_mac.CGContextDrawImage) = dlsym(g_mac.h_cg, "CGContextDrawImage");
    *(void **)(&g_mac.CGContextRelease) = dlsym(g_mac.h_cg, "CGContextRelease");
  }
  if (g_mac.h_cm) {
    *(void **)(&g_mac.CMSampleBufferGetImageBuffer) = dlsym(g_mac.h_cm, "CMSampleBufferGetImageBuffer");
    *(void **)(&g_mac.CMSampleBufferGetDataBuffer) = dlsym(g_mac.h_cm, "CMSampleBufferGetDataBuffer");
    *(void **)(&g_mac.CMBlockBufferGetDataPointer) = dlsym(g_mac.h_cm, "CMBlockBufferGetDataPointer");
  }
  if (g_mac.h_cv) {
    *(void **)(&g_mac.CVPixelBufferLockBaseAddress) = dlsym(g_mac.h_cv, "CVPixelBufferLockBaseAddress");
    *(void **)(&g_mac.CVPixelBufferUnlockBaseAddress) = dlsym(g_mac.h_cv, "CVPixelBufferUnlockBaseAddress");
    *(void **)(&g_mac.CVPixelBufferGetBaseAddress) = dlsym(g_mac.h_cv, "CVPixelBufferGetBaseAddress");
    *(void **)(&g_mac.CVPixelBufferGetWidth) = dlsym(g_mac.h_cv, "CVPixelBufferGetWidth");
    *(void **)(&g_mac.CVPixelBufferGetHeight) = dlsym(g_mac.h_cv, "CVPixelBufferGetHeight");
    *(void **)(&g_mac.CVPixelBufferGetBytesPerRow) = dlsym(g_mac.h_cv, "CVPixelBufferGetBytesPerRow");
  }
  *(void **)(&g_mac.dispatch_queue_create) = dlsym(RTLD_DEFAULT, "dispatch_queue_create");
  *(void **)(&g_mac.dispatch_sync_f) = dlsym(RTLD_DEFAULT, "dispatch_sync_f");
  *(void **)(&g_mac.dispatch_release) = dlsym(RTLD_DEFAULT, "dispatch_release");

  /* Dynamically register ObjC delegate class for AVCaptureVideoDataOutput / AVCaptureAudioDataOutput */
  if (g_mac.objc_getClass && g_mac.objc_allocateClassPair && g_mac.class_addMethod &&
      g_mac.class_addIvar && g_mac.objc_registerClassPair && g_mac.class_getInstanceVariable &&
      g_mac.ivar_getOffset) {
    FvClass existing = g_mac.objc_getClass("FvAvCaptureDelegate");
    if (existing) {
      g_mac.delegate_cls = existing;
    } else {
      FvClass ns_obj = g_mac.objc_getClass("NSObject");
      if (ns_obj) {
        FvClass cls = g_mac.objc_allocateClassPair(ns_obj, "FvAvCaptureDelegate", 0);
        if (cls) {
          g_mac.class_addIvar(cls, "_fvCtx", sizeof(void *), 3, "^v");
          FvSEL sel = g_mac.sel_registerName("captureOutput:didOutputSampleBuffer:fromConnection:");
          g_mac.class_addMethod(cls, sel, (void *)delegate_did_output_sample_buffer, "v@:@@^v@");
          g_mac.objc_registerClassPair(cls);
          g_mac.delegate_cls = cls;
        }
      }
    }
    if (g_mac.delegate_cls) {
      FvIvar iv = g_mac.class_getInstanceVariable(g_mac.delegate_cls, "_fvCtx");
      if (iv) g_mac.delegate_ctx_off = g_mac.ivar_getOffset(iv);
    }
  }
}

static void ensure_mac_av(void) {
  pthread_once(&g_mac_once, init_mac_av);
}

static int32_t get_av_auth_status(const char *media_type_fourcc) {
  if (!g_mac.h_avf || !g_mac.objc_getClass || !g_mac.objc_msgSend_ptr) return 0;
  FvClass dev_cls = g_mac.objc_getClass("AVCaptureDevice");
  if (!dev_cls) return 0;
  FvId mtype = ns_str(media_type_fourcc);
  if (!mtype) return 0;
  FvSEL sel = g_mac.sel_registerName("authorizationStatusForMediaType:");
  return (int32_t)msg1_long((FvId)dev_cls, sel, mtype);
}

static int append_line(char *out_buf, int32_t cap, int32_t *pos,
                       const char *kind, const char *id, const char *label, const char *group) {
  if (*pos >= cap - 1) return 0;
  int n = snprintf(out_buf + *pos, (size_t)(cap - *pos), "%s\t%s\t%s\t%s\n",
                   kind, id, label, group);
  if (n < 0) return 0;
  if (*pos + n >= cap) {
    *pos = cap - 1;
    return 0;
  }
  *pos += n;
  return 1;
}

static FvId discover_av_devices(const char *media_type_fourcc, const char *const *type_syms, int n_syms) {
  FvClass disc_cls = g_mac.objc_getClass("AVCaptureDeviceDiscoverySession");
  FvClass arr_cls = g_mac.objc_getClass("NSMutableArray");
  FvId mtype = ns_str(media_type_fourcc);
  if (!mtype) return NULL;

  if (disc_cls && arr_cls && g_mac.h_avf) {
    FvId types = msg0((FvId)arr_cls, g_mac.sel_registerName("array"));
    FvSEL add_sel = g_mac.sel_registerName("addObject:");
    int added = 0;
    for (int i = 0; i < n_syms; i++) {
      FvId *sym = (FvId *)dlsym(g_mac.h_avf, type_syms[i]);
      if (sym && *sym) {
        msg1_void(types, add_sel, *sym);
        added++;
      }
    }
    if (added > 0) {
      FvSEL dsel = g_mac.sel_registerName("discoverySessionWithDeviceTypes:mediaType:position:");
      FvId session = msg3_disc((FvId)disc_cls, dsel, types, mtype, 0 /* Unspecified */);
      if (session) {
        return msg0(session, g_mac.sel_registerName("devices"));
      }
    }
  }

  /* Fallback to +[AVCaptureDevice devicesWithMediaType:] */
  FvClass dev_cls = g_mac.objc_getClass("AVCaptureDevice");
  if (!dev_cls) return NULL;
  return msg1((FvId)dev_cls, g_mac.sel_registerName("devicesWithMediaType:"), mtype);
}

int32_t fv_av_macos_enumerate_devices(char *out_buf, int32_t cap, FvAvDeviceSummary *out_summary) {
  if (!out_buf || cap <= 1) return -1;
  ensure_mac_av();

  FvAvDeviceSummary sum;
  memset(&sum, 0, sizeof(sum));
  sum.avfoundation_loaded = (g_mac.h_avf && g_mac.objc_getClass && g_mac.objc_msgSend_ptr) ? 1 : 0;
  sum.coreaudio_loaded = (g_mac.h_ca && g_mac.AudioObjectGetPropertyDataSize &&
                          g_mac.AudioObjectGetPropertyData && g_mac.CFStringGetCString) ? 1 : 0;
  sum.coregraphics_loaded = (g_mac.h_cg && g_mac.CGMainDisplayID && g_mac.CGDisplayCreateImage) ? 1 : 0;
  sum.screencapturekit_loaded = g_mac.h_sck ? 1 : 0;
  sum.screen_tcc_granted = (g_mac.CGPreflightScreenCaptureAccess &&
                            g_mac.CGPreflightScreenCaptureAccess()) ? 1 : 0;

  int32_t pos = 0;
  out_buf[0] = '\0';

  /* Autorelease pool for Objective-C objects */
  FvId pool = NULL;
  if (g_mac.objc_getClass && g_mac.objc_msgSend_ptr) {
    FvClass pool_cls = g_mac.objc_getClass("NSAutoreleasePool");
    if (pool_cls) {
      pool = msg0(msg0((FvId)pool_cls, g_mac.sel_registerName("alloc")),
                  g_mac.sel_registerName("init"));
    }
  }

  if (sum.avfoundation_loaded) {
    sum.cam_auth_status = get_av_auth_status("vide");
    sum.mic_auth_status = get_av_auth_status("soun");

    /* 1. Real Microphones (AVMediaTypeAudio = "soun") */
    static const char *const mic_syms[] = {
      "AVCaptureDeviceTypeMicrophone",
      "AVCaptureDeviceTypeBuiltInMicrophone",
      "AVCaptureDeviceTypeExternal",
      "AVCaptureDeviceTypeExternalUnknown"
    };
    FvId mics = discover_av_devices("soun", mic_syms, 4);
    if (mics) {
      unsigned long cnt = msg0_ulong(mics, g_mac.sel_registerName("count"));
      FvSEL obj_at = g_mac.sel_registerName("objectAtIndex:");
      FvSEL uid_sel = g_mac.sel_registerName("uniqueID");
      FvSEL name_sel = g_mac.sel_registerName("localizedName");
      for (unsigned long i = 0; i < cnt; i++) {
        FvId dev = msg1_ulong(mics, obj_at, i);
        if (!dev) continue;
        char uid[256], name[256];
        ns_to_cstr(msg0(dev, uid_sel), uid, sizeof(uid));
        ns_to_cstr(msg0(dev, name_sel), name, sizeof(name));
        if (!uid[0]) snprintf(uid, sizeof(uid), "avf-mic-%lu", i);
        if (!name[0]) snprintf(name, sizeof(name), "AVFoundation Microphone %lu", i);
        append_line(out_buf, cap, &pos, "audioinput", uid, name, "avfoundation-audio");
        sum.real_mics++;
      }
    }

    /* 2. Real Cameras (AVMediaTypeVideo = "vide") */
    static const char *const cam_syms[] = {
      "AVCaptureDeviceTypeBuiltInWideAngleCamera",
      "AVCaptureDeviceTypeExternal",
      "AVCaptureDeviceTypeExternalUnknown",
      "AVCaptureDeviceTypeContinuityCamera",
      "AVCaptureDeviceTypeDeskViewCamera"
    };
    FvId cams = discover_av_devices("vide", cam_syms, 5);
    if (cams) {
      unsigned long cnt = msg0_ulong(cams, g_mac.sel_registerName("count"));
      FvSEL obj_at = g_mac.sel_registerName("objectAtIndex:");
      FvSEL uid_sel = g_mac.sel_registerName("uniqueID");
      FvSEL name_sel = g_mac.sel_registerName("localizedName");
      for (unsigned long i = 0; i < cnt; i++) {
        FvId dev = msg1_ulong(cams, obj_at, i);
        if (!dev) continue;
        char uid[256], name[256];
        ns_to_cstr(msg0(dev, uid_sel), uid, sizeof(uid));
        ns_to_cstr(msg0(dev, name_sel), name, sizeof(name));
        if (!uid[0]) snprintf(uid, sizeof(uid), "avf-cam-%lu", i);
        if (!name[0]) snprintf(name, sizeof(name), "AVFoundation Camera %lu", i);
        append_line(out_buf, cap, &pos, "videoinput", uid, name, "avfoundation-video");
        sum.real_cameras++;
      }
    }
  }

  /* 3. Real Speakers (CoreAudio HAL output devices) */
  if (sum.coreaudio_loaded) {
    FvAudioObjectPropertyAddress addr = {
      FV_K_AUDIO_PROP_DEVICES, FV_K_AUDIO_SCOPE_GLOBAL, 0
    };
    uint32_t data_size = 0;
    if (g_mac.AudioObjectGetPropertyDataSize(FV_K_AUDIO_OBJECT_SYSTEM, &addr, 0, NULL, &data_size) == 0 &&
        data_size >= sizeof(uint32_t)) {
      uint32_t count = data_size / (uint32_t)sizeof(uint32_t);
      if (count > 64) count = 64;
      uint32_t dev_ids[64];
      uint32_t io_size = count * (uint32_t)sizeof(uint32_t);
      if (g_mac.AudioObjectGetPropertyData(FV_K_AUDIO_OBJECT_SYSTEM, &addr, 0, NULL, &io_size, dev_ids) == 0) {
        count = io_size / (uint32_t)sizeof(uint32_t);
        for (uint32_t i = 0; i < count; i++) {
          FvAudioObjectPropertyAddress stream_addr = {
            FV_K_AUDIO_PROP_STREAMS, FV_K_AUDIO_SCOPE_OUTPUT, 0
          };
          uint32_t stream_sz = 0;
          if (g_mac.AudioObjectGetPropertyDataSize(dev_ids[i], &stream_addr, 0, NULL, &stream_sz) != 0 ||
              stream_sz == 0) {
            continue; /* Not an output device */
          }

          char name[256] = {0};
          char uid[256] = {0};

          FvCFStringRef cf_name = NULL;
          uint32_t sz = (uint32_t)sizeof(cf_name);
          FvAudioObjectPropertyAddress name_addr = {
            FV_K_AUDIO_PROP_NAME, FV_K_AUDIO_SCOPE_GLOBAL, 0
          };
          if (g_mac.AudioObjectGetPropertyData(dev_ids[i], &name_addr, 0, NULL, &sz, &cf_name) == 0 && cf_name) {
            g_mac.CFStringGetCString(cf_name, name, (long)sizeof(name), FV_K_CF_STRING_ENCODING_UTF8);
            g_mac.CFRelease(cf_name);
          }

          FvCFStringRef cf_uid = NULL;
          sz = (uint32_t)sizeof(cf_uid);
          FvAudioObjectPropertyAddress uid_addr = {
            FV_K_AUDIO_PROP_UID, FV_K_AUDIO_SCOPE_GLOBAL, 0
          };
          if (g_mac.AudioObjectGetPropertyData(dev_ids[i], &uid_addr, 0, NULL, &sz, &cf_uid) == 0 && cf_uid) {
            g_mac.CFStringGetCString(cf_uid, uid, (long)sizeof(uid), FV_K_CF_STRING_ENCODING_UTF8);
            g_mac.CFRelease(cf_uid);
          }

          for (char *p = name; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
          for (char *p = uid; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
          if (!uid[0]) snprintf(uid, sizeof(uid), "coreaudio-out-%u", dev_ids[i]);
          if (!name[0]) snprintf(name, sizeof(name), "CoreAudio Output %u", dev_ids[i]);
          append_line(out_buf, cap, &pos, "audiooutput", uid, name, "coreaudio-output");
          sum.real_speakers++;
        }
      }
    }
  }

  /* Explicitly emit virtual/fallback entries for any category with 0 real hardware devices */
  if (sum.real_mics == 0) {
    const char *reason = !sum.avfoundation_loaded ? "avfoundation unavailable"
                       : (sum.mic_auth_status == 2 || sum.mic_auth_status == 1) ? "tcc denied"
                       : "no hardware microphone";
    char lbl[160];
    snprintf(lbl, sizeof(lbl), "Farview Virtual Microphone (fallback: %s)", reason);
    append_line(out_buf, cap, &pos, "audioinput", "fv-mic-fallback", lbl, "fv-grp-virtual");
  }
  if (sum.real_cameras == 0) {
    const char *reason = !sum.avfoundation_loaded ? "avfoundation unavailable"
                       : (sum.cam_auth_status == 2 || sum.cam_auth_status == 1) ? "tcc denied"
                       : "no hardware camera";
    char lbl[160];
    snprintf(lbl, sizeof(lbl), "Farview Virtual Camera (fallback: %s)", reason);
    append_line(out_buf, cap, &pos, "videoinput", "fv-cam-fallback", lbl, "fv-grp-virtual");
  }
  if (sum.real_speakers == 0) {
    const char *reason = !sum.coreaudio_loaded ? "coreaudio unavailable" : "no hardware speaker";
    char lbl[160];
    snprintf(lbl, sizeof(lbl), "Farview Virtual Speaker (fallback: %s)", reason);
    append_line(out_buf, cap, &pos, "audiooutput", "fv-spk-fallback", lbl, "fv-grp-virtual");
  }

  if (pool) {
    msg0_void(pool, g_mac.sel_registerName("drain"));
  }

  if (out_summary) *out_summary = sum;
  return pos;
}

static int capture_screen_cg_once(int32_t width, int32_t height, uint8_t *out_buf, int32_t buf_cap) {
  if (!g_mac.CGMainDisplayID || !g_mac.CGDisplayCreateImage || !g_mac.CGColorSpaceCreateDeviceRGB ||
      !g_mac.CGBitmapContextCreate || !g_mac.CGContextDrawImage || !g_mac.CGContextRelease ||
      !g_mac.CGImageRelease || !g_mac.CGColorSpaceRelease) {
    return -1;
  }
  int64_t need64 = (int64_t)width * (int64_t)height * 4;
  if (need64 <= 0 || need64 > buf_cap) return -1;

  FvCGDirectDisplayID disp = g_mac.CGMainDisplayID();
  FvCGImageRef img = g_mac.CGDisplayCreateImage(disp);
  if (!img) return -1;

  FvCGColorSpaceRef cs = g_mac.CGColorSpaceCreateDeviceRGB();
  if (!cs) {
    g_mac.CGImageRelease(img);
    return -1;
  }

  FvCGContextRef ctx = g_mac.CGBitmapContextCreate(
      out_buf, (size_t)width, (size_t)height, 8, (size_t)width * 4, cs,
      FV_K_CG_ALPHA_PREMULT_LAST | FV_K_CG_BYTEORDER_32BIG);
  if (!ctx) {
    g_mac.CGColorSpaceRelease(cs);
    g_mac.CGImageRelease(img);
    return -1;
  }

  FvCGRect rect = { { 0.0, 0.0 }, { (double)width, (double)height } };
  g_mac.CGContextDrawImage(ctx, rect, img);

  g_mac.CGContextRelease(ctx);
  g_mac.CGColorSpaceRelease(cs);
  g_mac.CGImageRelease(img);
  return (int)need64;
}

FvMacCapture *fv_av_macos_capture_open(int32_t kind, int32_t width, int32_t height,
                                       int32_t *out_mode, int32_t *out_reason) {
  if (out_mode) *out_mode = FV_CAP_MODE_FALLBACK;
  if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;

  const char *force_fb = getenv("FV_CAPTURE_FORCE_FALLBACK");
  if (force_fb && strcmp(force_fb, "1") == 0) {
    if (out_reason) *out_reason = FV_CAP_REASON_FORCED_FALLBACK;
    return NULL;
  }

  ensure_mac_av();

  /* --- Screen capture (CoreGraphics) --- */
  if (kind == 2 /* FV_CAPTURE_SCREEN */) {
    if (!g_mac.h_cg || !g_mac.CGMainDisplayID || !g_mac.CGDisplayCreateImage) {
      if (out_reason) *out_reason = FV_CAP_REASON_FRAMEWORK_MISSING;
      return NULL;
    }
    if (g_mac.CGPreflightScreenCaptureAccess && !g_mac.CGPreflightScreenCaptureAccess()) {
      if (out_reason) *out_reason = FV_CAP_REASON_PERMISSION_DENIED;
      return NULL;
    }
    /* Verify we can actually grab a frame without error */
    FvCGImageRef probe = g_mac.CGDisplayCreateImage(g_mac.CGMainDisplayID());
    if (!probe) {
      if (out_reason) *out_reason = FV_CAP_REASON_PERMISSION_DENIED;
      return NULL;
    }
    g_mac.CGImageRelease(probe);

    FvMacCapture *cap = (FvMacCapture *)calloc(1, sizeof(FvMacCapture));
    if (!cap) return NULL;
    pthread_mutex_init(&cap->mu, NULL);
    cap->kind = kind;
    cap->mode = FV_CAP_MODE_COREGRAPHICS;
    cap->width = width;
    cap->height = height;
    if (out_mode) *out_mode = FV_CAP_MODE_COREGRAPHICS;
    if (out_reason) *out_reason = FV_CAP_REASON_OK;
    return cap;
  }

  /* --- Camera (1) / Microphone (0) via AVFoundation --- */
  if (!g_mac.h_avf || !g_mac.objc_getClass || !g_mac.objc_msgSend_ptr ||
      !g_mac.delegate_cls || !g_mac.dispatch_queue_create) {
    if (out_reason) *out_reason = FV_CAP_REASON_FRAMEWORK_MISSING;
    return NULL;
  }

  const char *fourcc = (kind == 1) ? "vide" : "soun";
  FvId pool = NULL;
  FvClass pool_cls = g_mac.objc_getClass("NSAutoreleasePool");
  if (pool_cls) {
    pool = msg0(msg0((FvId)pool_cls, g_mac.sel_registerName("alloc")),
                g_mac.sel_registerName("init"));
  }

  /* Discover physical device first (read-only query, does not prompt TCC) */
  FvClass dev_cls = g_mac.objc_getClass("AVCaptureDevice");
  FvId mtype = ns_str(fourcc);
  FvId dev = dev_cls ? msg1((FvId)dev_cls, g_mac.sel_registerName("defaultDeviceWithMediaType:"), mtype) : NULL;
  if (!dev) {
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_NO_DEVICE;
    return NULL;
  }

  /* Check TCC authorization before opening device input (never crash headless CLI on NotDetermined) */
  int32_t auth = get_av_auth_status(fourcc);
  if (auth == 1 || auth == 2) {
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_PERMISSION_DENIED;
    return NULL;
  }
  if (auth == 0) {
    const char *allow_prompt = getenv("FV_ALLOW_TCC_PROMPT");
    if (!allow_prompt || strcmp(allow_prompt, "1") != 0) {
      if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
      if (out_reason) *out_reason = FV_CAP_REASON_TCC_NOT_DETERMINED;
      return NULL;
    }
  }

  FvClass input_cls = g_mac.objc_getClass("AVCaptureDeviceInput");
  FvId err = NULL;
  FvId input = input_cls ? msg2_err((FvId)input_cls,
                                    g_mac.sel_registerName("deviceInputWithDevice:error:"),
                                    dev, &err) : NULL;
  if (!input) {
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;
    return NULL;
  }

  FvClass sess_cls = g_mac.objc_getClass("AVCaptureSession");
  FvId session = sess_cls ? msg0(msg0((FvId)sess_cls, g_mac.sel_registerName("alloc")),
                                 g_mac.sel_registerName("init")) : NULL;
  if (!session) {
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;
    return NULL;
  }

  if (!msg1_bool(session, g_mac.sel_registerName("canAddInput:"), input)) {
    msg0_void(session, g_mac.sel_registerName("release"));
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;
    return NULL;
  }
  msg1_void(session, g_mac.sel_registerName("addInput:"), input);

  FvClass out_cls = g_mac.objc_getClass(kind == 1 ? "AVCaptureVideoDataOutput" : "AVCaptureAudioDataOutput");
  FvId output = out_cls ? msg0(msg0((FvId)out_cls, g_mac.sel_registerName("alloc")),
                               g_mac.sel_registerName("init")) : NULL;
  if (!output) {
    msg0_void(session, g_mac.sel_registerName("release"));
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;
    return NULL;
  }

  if (kind == 1) {
    /* Request 32BGRA pixel format */
    FvId *pf_key = (FvId *)dlsym(g_mac.h_cv, "kCVPixelBufferPixelFormatTypeKey");
    FvClass num_cls = g_mac.objc_getClass("NSNumber");
    FvClass dict_cls = g_mac.objc_getClass("NSDictionary");
    if (pf_key && *pf_key && num_cls && dict_cls) {
      FvId num = msg1_u32((FvId)num_cls, g_mac.sel_registerName("numberWithUnsignedInt:"), FV_K_CV_PIXFMT_32BGRA);
      FvId settings = msg2((FvId)dict_cls, g_mac.sel_registerName("dictionaryWithObject:forKey:"), num, *pf_key);
      if (settings) {
        msg1_void(output, g_mac.sel_registerName("setVideoSettings:"), settings);
      }
    }
    msg1_bool_void(output, g_mac.sel_registerName("setAlwaysDiscardsLateVideoFrames:"), true);
  }

  if (!msg1_bool(session, g_mac.sel_registerName("canAddOutput:"), output)) {
    msg0_void(output, g_mac.sel_registerName("release"));
    msg0_void(session, g_mac.sel_registerName("release"));
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    if (out_reason) *out_reason = FV_CAP_REASON_SESSION_ERROR;
    return NULL;
  }

  FvMacCapture *cap = (FvMacCapture *)calloc(1, sizeof(FvMacCapture));
  if (!cap) {
    msg0_void(output, g_mac.sel_registerName("release"));
    msg0_void(session, g_mac.sel_registerName("release"));
    if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));
    return NULL;
  }
  pthread_mutex_init(&cap->mu, NULL);
  cap->kind = kind;
  cap->mode = FV_CAP_MODE_AVFOUNDATION;
  cap->width = width;
  cap->height = height;
  cap->frame_cap = (kind == 1) ? (width * height * 4) : 16384;
  cap->frame_buf = (uint8_t *)calloc(1, (size_t)cap->frame_cap);

  FvId delegate = msg0(msg0((FvId)g_mac.delegate_cls, g_mac.sel_registerName("alloc")),
                       g_mac.sel_registerName("init"));
  memcpy((uint8_t *)delegate + g_mac.delegate_ctx_off, &cap, sizeof(cap));

  FvDispatchQueue q = g_mac.dispatch_queue_create("farview.av.capture", NULL);
  msg2_void(output, g_mac.sel_registerName("setSampleBufferDelegate:queue:"), delegate, (FvId)q);
  msg1_void(session, g_mac.sel_registerName("addOutput:"), output);
  msg0_void(output, g_mac.sel_registerName("release")); /* session retains output */

  cap->session = session;
  cap->delegate = delegate;
  cap->queue = q;

  msg0_void(session, g_mac.sel_registerName("startRunning"));
  if (pool) msg0_void(pool, g_mac.sel_registerName("drain"));

  if (out_mode) *out_mode = FV_CAP_MODE_AVFOUNDATION;
  if (out_reason) *out_reason = FV_CAP_REASON_OK;
  return cap;
}

int32_t fv_av_macos_capture_read_frame(FvMacCapture *cap, uint8_t *out_buf, int32_t buf_cap) {
  if (!cap || !out_buf || buf_cap <= 0) return -1;

  if (cap->mode == FV_CAP_MODE_COREGRAPHICS) {
    return (int32_t)capture_screen_cg_once(cap->width, cap->height, out_buf, buf_cap);
  }

  /* Wait briefly on initial start for first hardware frame (up to 100ms) */
  for (int attempt = 0; attempt < 10; attempt++) {
    pthread_mutex_lock(&cap->mu);
    if (cap->frame_len > 0 && cap->frame_buf) {
      int32_t n = cap->frame_len < buf_cap ? cap->frame_len : buf_cap;
      memcpy(out_buf, cap->frame_buf, (size_t)n);
      pthread_mutex_unlock(&cap->mu);
      return n;
    }
    pthread_mutex_unlock(&cap->mu);
    usleep(10000);
  }
  return 0;
}

static void clear_delegate_ctx_barrier(void *ctx) {
  FvId delegate = (FvId)ctx;
  if (delegate && g_mac.delegate_ctx_off > 0) {
    FvMacCapture *null_ptr = NULL;
    memcpy((uint8_t *)delegate + g_mac.delegate_ctx_off, &null_ptr, sizeof(null_ptr));
  }
}

void fv_av_macos_capture_close(FvMacCapture *cap) {
  if (!cap) return;
  if (cap->session) {
    msg0_void(cap->session, g_mac.sel_registerName("stopRunning"));
  }
  /* Drain any in-flight serial GCD sample-buffer callbacks and clear _fvCtx
   * inside the serial queue so no callback can dereference cap after free(). */
  if (cap->queue && g_mac.dispatch_sync_f) {
    g_mac.dispatch_sync_f(cap->queue, cap->delegate, clear_delegate_ctx_barrier);
  } else {
    clear_delegate_ctx_barrier(cap->delegate);
  }
  if (cap->session) {
    msg0_void(cap->session, g_mac.sel_registerName("release"));
    cap->session = NULL;
  }
  if (cap->delegate) {
    msg0_void(cap->delegate, g_mac.sel_registerName("release"));
    cap->delegate = NULL;
  }
  if (cap->queue && g_mac.dispatch_release) {
    g_mac.dispatch_release(cap->queue);
    cap->queue = NULL;
  }
  pthread_mutex_destroy(&cap->mu);
  if (cap->frame_buf) {
    free(cap->frame_buf);
    cap->frame_buf = NULL;
  }
  free(cap);
}

#else /* !__APPLE__ (Linux / other OS) */

int32_t fv_av_macos_enumerate_devices(char *out_buf, int32_t cap, FvAvDeviceSummary *out_summary) {
  if (!out_buf || cap <= 1) return -1;
  if (out_summary) memset(out_summary, 0, sizeof(*out_summary));
  int n = snprintf(
      out_buf, (size_t)cap,
      "audioinput\tfv-mic-fallback\tFarview Virtual Microphone (fallback: non-macos)\tfv-grp-virtual\n"
      "videoinput\tfv-cam-fallback\tFarview Virtual Camera (fallback: non-macos)\tfv-grp-virtual\n"
      "audiooutput\tfv-spk-fallback\tFarview Virtual Speaker (fallback: non-macos)\tfv-grp-virtual\n");
  if (n < 0) return -1;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

FvMacCapture *fv_av_macos_capture_open(int32_t kind, int32_t width, int32_t height,
                                       int32_t *out_mode, int32_t *out_reason) {
  (void)kind;
  (void)width;
  (void)height;
  if (out_mode) *out_mode = FV_CAP_MODE_FALLBACK;
  if (out_reason) *out_reason = FV_CAP_REASON_UNSUPPORTED_OS;
  return NULL;
}

int32_t fv_av_macos_capture_read_frame(FvMacCapture *cap, uint8_t *out_buf, int32_t buf_cap) {
  (void)cap;
  (void)out_buf;
  (void)buf_cap;
  return -1;
}

void fv_av_macos_capture_close(FvMacCapture *cap) {
  (void)cap;
}

#endif /* __APPLE__ */
