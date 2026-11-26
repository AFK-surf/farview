// Farview Wave M1: In-process hardware-accelerated (VideoToolbox on macOS with SW fallback)
// streaming A/V decoder engine using dlopen/dlsym on FFmpeg shared libraries
// (libavformat / libavcodec / libswscale / libswresample / libavutil).
//
// Zero compile-time dependency on libav*-dev headers or libraries: compiles cleanly on minimal
// Linux and macOS environments, and gracefully falls back to media_stub.c CLI ffmpeg when
// shared libraries are absent.
#include "media_av.h"
#include "audio_out.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../win_compat.h"
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#define FV_AV_MAX_STREAMS 16
#define FV_AV_IO_BUF_SIZE 32768
#define FV_AV_MEM_MAX_BYTES (64 * 1024 * 1024)
#define FV_AV_DECODE_AUDIO_MAX_SAMPLES (48000 * 600) // cap at 10 min @ 48kHz per channel

// Standard FFmpeg constants that have been ABI-stable across FFmpeg 4..9.
#define FV_AVMEDIA_TYPE_VIDEO 0
#define FV_AVMEDIA_TYPE_AUDIO 1
#define FV_AVSEEK_SIZE 0x10000
#define FV_AVSEEK_FLAG_BACKWARD 1
#define FV_AV_TIME_BASE 1000000
#define FV_AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
#define FV_AV_LOG_QUIET -8
#define FV_AV_LOG_ERROR 16
#define FV_SWS_BILINEAR 2
#define FV_AVERROR_EOF (-((int)('E' | ('O' << 8) | ('F' << 16) | (' ' << 24))))
#define FV_AVERROR_EAGAIN (-EAGAIN)

typedef struct {
  int num;
  int den;
} FvAVRational;

typedef struct {
  int order;
  int nb_channels;
  union {
    uint64_t mask;
    void *map;
  } u;
  void *opaque;
} FvAVChannelLayout;

typedef struct {
  const char *name;
  const char *help;
  int offset;
  int type;
  union {
    int64_t i64;
    double dbl;
    const char *str;
    FvAVRational q;
  } default_val;
  double min;
  double max;
  int flags;
  const char *unit;
} FvAVOption;

// ABI field offsets resolved at init time from major versions + self-calibration.
typedef struct {
  // AVFormatContext
  size_t fc_pb;
  size_t fc_nb_streams;
  size_t fc_streams;
  size_t fc_start_time;
  size_t fc_duration;
  // AVStream
  size_t st_index;
  size_t st_codecpar;
  size_t st_time_base;
  size_t st_start_time;
  size_t st_duration;
  size_t st_avg_frame_rate;
  // AVCodecParameters
  size_t cp_codec_type;
  size_t cp_codec_id;
  size_t cp_format;
  size_t cp_width;
  size_t cp_height;
  size_t cp_ch_layout;
  size_t cp_sample_rate;
  // AVCodecContext
  size_t cc_time_base;
  size_t cc_pkt_timebase;
  size_t cc_width;
  size_t cc_height;
  size_t cc_pix_fmt;
  size_t cc_sample_rate;
  size_t cc_sample_fmt;
  size_t cc_ch_layout;
  size_t cc_thread_count;
  size_t cc_hw_device_ctx; // 0 if unknown/disabled
  // AVPacket (frozen across FFmpeg 4..9)
  size_t pkt_pts;
  size_t pkt_dts;
  size_t pkt_data;
  size_t pkt_size;
  size_t pkt_stream_index;
  size_t pkt_duration;
  // AVFrame
  size_t fr_data;
  size_t fr_linesize;
  size_t fr_width;
  size_t fr_height;
  size_t fr_nb_samples;
  size_t fr_format;
  size_t fr_pts;
  size_t fr_sample_rate;
  size_t fr_ch_layout;
  // AVIOContext
  size_t io_buffer;
} FvAvAbi;

typedef struct {
  int initialized;
  int available;
  int avf_major;
  int avc_major;
  int avu_major;

  void *h_avutil;
  void *h_avcodec;
  void *h_avformat;
  void *h_swscale;
  void *h_swresample;

  FvAvAbi abi;

  // Resolved runtime enum IDs (never hardcoded).
  int pix_fmt_rgba;
  int pix_fmt_yuv420p;
  int pix_fmt_yuvj420p;
  int pix_fmt_nv12;
  int pix_fmt_videotoolbox;
  int sample_fmt_s16;
  int sample_fmt_fltp;
  int hw_type_videotoolbox;

  // libavutil
  unsigned (*avutil_version)(void);
  void *(*av_malloc)(size_t size);
  void (*av_free)(void *ptr);
  void (*av_freep)(void *ptr);
  void (*av_log_set_level)(int level);
  void *(*av_frame_alloc)(void);
  void (*av_frame_free)(void **frame);
  void (*av_frame_unref)(void *frame);
  int (*av_frame_get_buffer)(void *frame, int align);
  int (*av_get_pix_fmt)(const char *name);
  int (*av_get_sample_fmt)(const char *name);
  int (*av_hwdevice_find_type_by_name)(const char *name);
  int (*av_hwdevice_ctx_create)(void **device_ctx, int type, const char *device, void *opts, int flags);
  int (*av_hwframe_transfer_data)(void *dst, const void *src, int flags);
  void *(*av_buffer_ref)(void *buf);
  void (*av_buffer_unref)(void **buf);
  int64_t (*av_rescale_q)(int64_t a, FvAVRational bq, FvAVRational cq);
  void (*av_channel_layout_default)(FvAVChannelLayout *ch_layout, int nb_channels);
  void (*av_channel_layout_uninit)(FvAVChannelLayout *ch_layout);
  int (*av_channel_layout_copy)(FvAVChannelLayout *dst, const FvAVChannelLayout *src);
  const FvAVOption *(*av_opt_find)(void *obj, const char *name, const char *unit, int opt_flags, int search_flags);
  int (*av_opt_set_int)(void *obj, const char *name, int64_t val, int search_flags);
  int (*av_dict_set)(void **pm, const char *key, const char *value, int flags);
  void (*av_dict_free)(void **m);

  // libavcodec
  unsigned (*avcodec_version)(void);
  const void *(*avcodec_find_decoder)(int id);
  const void *(*avcodec_find_encoder_by_name)(const char *name);
  const char *(*avcodec_get_name)(int id);
  void *(*avcodec_alloc_context3)(const void *codec);
  void (*avcodec_free_context)(void **avctx);
  void *(*avcodec_parameters_alloc)(void);
  void (*avcodec_parameters_free)(void **par);
  int (*avcodec_parameters_to_context)(void *codec, const void *par);
  int (*avcodec_open2)(void *avctx, const void *codec, void **options);
  void (*avcodec_flush_buffers)(void *avctx);
  int (*avcodec_send_packet)(void *avctx, const void *avpkt);
  int (*avcodec_receive_frame)(void *avctx, void *frame);
  int (*avcodec_send_frame)(void *avctx, const void *frame);
  int (*avcodec_receive_packet)(void *avctx, void *avpkt);
  void *(*av_packet_alloc)(void);
  void (*av_packet_free)(void **pkt);
  void (*av_packet_unref)(void *pkt);

  // libavformat
  unsigned (*avformat_version)(void);
  void *(*avformat_alloc_context)(void);
  void (*avformat_free_context)(void *s);
  int (*avformat_open_input)(void **ps, const char *url, const void *fmt, void **options);
  void (*avformat_close_input)(void **s);
  int (*avformat_find_stream_info)(void *ic, void **options);
  int (*av_find_best_stream)(void *ic, int type, int wanted_stream_nb, int related_stream, const void **decoder_ret, int flags);
  int (*av_read_frame)(void *s, void *pkt);
  int (*av_seek_frame)(void *s, int stream_index, int64_t timestamp, int flags);
  void *(*avio_alloc_context)(unsigned char *buffer, int buffer_size, int write_flag, void *opaque,
                              int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                              int (*write_packet)(void *opaque, const uint8_t *buf, int buf_size),
                              int64_t (*seek)(void *opaque, int64_t offset, int whence));
  void (*avio_context_free)(void **s);

  // libswscale
  void *(*sws_getContext)(int srcW, int srcH, int srcFormat, int dstW, int dstH, int dstFormat,
                          int flags, void *srcFilter, void *dstFilter, const double *param);
  int (*sws_scale)(void *c, const uint8_t *const srcSlice[], const int srcStride[],
                   int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
  void (*sws_freeContext)(void *swsContext);

  // libswresample
  int (*swr_alloc_set_opts2)(void **ps, const FvAVChannelLayout *out_ch_layout, int out_sample_fmt, int out_sample_rate,
                             const FvAVChannelLayout *in_ch_layout, int in_sample_fmt, int in_sample_rate,
                             int log_offset, void *log_ctx);
  int (*swr_init)(void *s);
  int (*swr_convert)(void *s, uint8_t **out, int out_count, const uint8_t **in, int in_count);
  int64_t (*swr_get_delay)(void *s, int64_t base);
  void (*swr_free)(void **s);
} FvAvLib;

static FvAvLib g_av;
static pthread_once_t g_av_once = PTHREAD_ONCE_INIT;

// Inline typed field accessors
static inline int32_t rd_i32(const void *p, size_t off) {
  int32_t v; memcpy(&v, (const uint8_t *)p + off, sizeof(v)); return v;
}
static inline uint32_t rd_u32(const void *p, size_t off) {
  uint32_t v; memcpy(&v, (const uint8_t *)p + off, sizeof(v)); return v;
}
static inline int64_t rd_i64(const void *p, size_t off) {
  int64_t v; memcpy(&v, (const uint8_t *)p + off, sizeof(v)); return v;
}
static inline void *rd_ptr(const void *p, size_t off) {
  void *v; memcpy(&v, (const uint8_t *)p + off, sizeof(v)); return v;
}
static inline FvAVRational rd_rat(const void *p, size_t off) {
  FvAVRational v; memcpy(&v, (const uint8_t *)p + off, sizeof(v)); return v;
}
static inline void wr_i32(void *p, size_t off, int32_t v) {
  memcpy((uint8_t *)p + off, &v, sizeof(v));
}
static inline void wr_i64(void *p, size_t off, int64_t v) {
  memcpy((uint8_t *)p + off, &v, sizeof(v));
}
static inline void wr_ptr(void *p, size_t off, void *v) {
  memcpy((uint8_t *)p + off, &v, sizeof(v));
}
static inline void wr_rat(void *p, size_t off, FvAVRational v) {
  memcpy((uint8_t *)p + off, &v, sizeof(v));
}

static void *try_dlopen_list(const char *const *names) {
  for (int i = 0; names[i]; i++) {
    void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (h) return h;
  }
  return NULL;
}

#define LOAD_SYM(lib, field) do { \
  *(void **)(&g_av.field) = dlsym(lib, #field); \
  if (!g_av.field) return; \
} while (0)

#define LOAD_OPT_SYM(lib, field) do { \
  *(void **)(&g_av.field) = dlsym(lib, #field); \
} while (0)

static int calibrate_abi(void) {
  int avf = g_av.avf_major;
  int avc = g_av.avc_major;
  int avu = g_av.avu_major;

  // Supported major versions: FFmpeg 6 (60/60/58), 7 (61/61/59), 8 (62/62/60), 9 (63/63/61).
  if (avf < 60 || avf > 63 || avc < 60 || avc > 63 || avu < 58 || avu > 61) {
    return 0;
  }

  FvAvAbi *a = &g_av.abi;
  memset(a, 0, sizeof(*a));

  // AVFormatContext
  a->fc_pb = 32;
  a->fc_nb_streams = 44;
  a->fc_streams = 48;
  if (avf >= 61) {
    // FFmpeg 7/8/9: nb_stream_groups + stream_groups added before nb_chapters
    a->fc_start_time = 96;
    a->fc_duration = 104;
  } else {
    // FFmpeg 6
    a->fc_start_time = 80;
    a->fc_duration = 88;
  }

  // AVStream
  if (avf >= 61) {
    // FFmpeg 7/8/9: av_class at 0
    a->st_index = 8;
    a->st_codecpar = 16;
    a->st_time_base = 32;
    a->st_start_time = 40;
    a->st_duration = 48;
    a->st_avg_frame_rate = 88;
  } else {
    // FFmpeg 6: no av_class
    a->st_index = 0;
    a->st_codecpar = 8;
    a->st_time_base = 24;
    a->st_start_time = 32;
    a->st_duration = 40;
    a->st_avg_frame_rate = 80;
  }

  // AVCodecParameters
  a->cp_codec_type = 0;
  a->cp_codec_id = 4;
  if (avc >= 61) {
    a->cp_format = 44;
    a->cp_width = 72;
    a->cp_height = 76;
    a->cp_ch_layout = 128;
    a->cp_sample_rate = 152;
  } else {
    // FFmpeg 6.0 vs 6.1: coded_side_data added in 60.31
    unsigned v = g_av.avcodec_version();
    int avc_minor = (int)((v >> 8) & 0xFF);
    if (avc_minor >= 31) {
      a->cp_format = 44;
      a->cp_width = 72;
      a->cp_height = 76;
      a->cp_ch_layout = 128;
      a->cp_sample_rate = 152;
    } else {
      a->cp_format = 28;
      a->cp_width = 56;
      a->cp_height = 60;
      a->cp_ch_layout = 112;
      a->cp_sample_rate = 136;
    }
  }

  // AVPacket (frozen across FFmpeg 4..9)
  a->pkt_pts = 8;
  a->pkt_dts = 16;
  a->pkt_data = 24;
  a->pkt_size = 32;
  a->pkt_stream_index = 36;
  a->pkt_duration = 64;

  // AVFrame
  a->fr_data = 0;
  a->fr_linesize = 64;
  a->fr_width = 104;
  a->fr_height = 108;
  a->fr_nb_samples = 112;
  a->fr_format = 116;
  if (avu >= 59) {
    // FFmpeg 7/8/9 (avutil 59/60/61): key_frame removed
    a->fr_pts = 136;
    a->fr_sample_rate = 180;
    a->fr_ch_layout = (avu >= 60) ? 384 : 392; // verified 384 on avutil 61
  } else {
    // FFmpeg 6 (avutil 58)
    a->fr_pts = 144;
    a->fr_sample_rate = 192;
    a->fr_ch_layout = 424;
  }

  // AVIOContext
  a->io_buffer = 8;

  // Dynamically discover AVCodecContext offsets using av_opt_find + avcodec_parameters_to_context!
  void *cc = g_av.avcodec_alloc_context3(NULL);
  void *cp = g_av.avcodec_parameters_alloc();
  if (!cc || !cp) {
    if (cp) g_av.avcodec_parameters_free(&cp);
    if (cc) g_av.avcodec_free_context(&cc);
    return 0;
  }

  const FvAVOption *opt_tb = g_av.av_opt_find(cc, "time_base", NULL, 0, 0);
  const FvAVOption *opt_ch = g_av.av_opt_find(cc, "ch_layout", NULL, 0, 0);
  const FvAVOption *opt_th = g_av.av_opt_find(cc, "threads", NULL, 0, 0);
  if (!opt_tb || !opt_ch || !opt_th) {
    g_av.avcodec_parameters_free(&cp);
    g_av.avcodec_free_context(&cc);
    return 0;
  }
  a->cc_time_base = (size_t)opt_tb->offset;
  a->cc_pkt_timebase = a->cc_time_base + sizeof(FvAVRational);
  a->cc_ch_layout = (size_t)opt_ch->offset;
  a->cc_thread_count = (size_t)opt_th->offset;

  // Calibrate width, height, pix_fmt via avcodec_parameters_to_context with sentinel values.
  wr_i32(cp, a->cp_codec_type, FV_AVMEDIA_TYPE_VIDEO);
  wr_i32(cp, a->cp_width, 0x1357);
  wr_i32(cp, a->cp_height, 0x2468);
  wr_i32(cp, a->cp_format, 0x1A2B);
  if (g_av.avcodec_parameters_to_context(cc, cp) < 0) {
    g_av.avcodec_parameters_free(&cp);
    g_av.avcodec_free_context(&cc);
    return 0;
  }
  for (size_t off = a->cc_time_base; off + 8 < 512; off += 4) {
    if (rd_i32(cc, off) == 0x1357 && rd_i32(cc, off + 4) == 0x2468 && a->cc_width == 0) {
      a->cc_width = off;
      a->cc_height = off + 4;
    }
    if (rd_i32(cc, off) == 0x1A2B && a->cc_pix_fmt == 0) {
      a->cc_pix_fmt = off;
    }
  }

  // Calibrate sample_rate, sample_fmt via avcodec_parameters_to_context.
  wr_i32(cp, a->cp_codec_type, FV_AVMEDIA_TYPE_AUDIO);
  wr_i32(cp, a->cp_sample_rate, 44123);
  wr_i32(cp, a->cp_format, 0x2C3D);
  if (g_av.avcodec_parameters_to_context(cc, cp) >= 0) {
    for (size_t off = (a->cc_ch_layout > 32 ? a->cc_ch_layout - 32 : 0); off < a->cc_ch_layout; off += 4) {
      if (rd_i32(cc, off) == 44123) a->cc_sample_rate = off;
      if (rd_i32(cc, off) == 0x2C3D) a->cc_sample_fmt = off;
    }
  }

  g_av.avcodec_parameters_free(&cp);
  g_av.avcodec_free_context(&cc);

  if (a->cc_width == 0 || a->cc_height == 0 || a->cc_pix_fmt == 0 ||
      a->cc_sample_rate == 0 || a->cc_sample_fmt == 0) {
    return 0;
  }

  // Verified hw_device_ctx offset on macOS Homebrew FFmpeg 9 (avcodec 63).
#ifdef __APPLE__
  if (avc == 63) {
    a->cc_hw_device_ctx = 560;
  }
#endif

  return 1;
}

static void fv_av_do_init(void) {
  g_av.initialized = 1;
  g_av.available = 0;

  if (getenv("FV_DISABLE_INPROC_MEDIA") && strcmp(getenv("FV_DISABLE_INPROC_MEDIA"), "1") == 0) {
    return;
  }

#ifdef __APPLE__
  static const char *const avutil_names[] = {
    "/opt/homebrew/lib/libavutil.dylib",
    "/usr/local/lib/libavutil.dylib",
    "libavutil.dylib",
    NULL
  };
  static const char *const avcodec_names[] = {
    "/opt/homebrew/lib/libavcodec.dylib",
    "/usr/local/lib/libavcodec.dylib",
    "libavcodec.dylib",
    NULL
  };
  static const char *const avformat_names[] = {
    "/opt/homebrew/lib/libavformat.dylib",
    "/usr/local/lib/libavformat.dylib",
    "libavformat.dylib",
    NULL
  };
  static const char *const swscale_names[] = {
    "/opt/homebrew/lib/libswscale.dylib",
    "/usr/local/lib/libswscale.dylib",
    "libswscale.dylib",
    NULL
  };
  static const char *const swresample_names[] = {
    "/opt/homebrew/lib/libswresample.dylib",
    "/usr/local/lib/libswresample.dylib",
    "libswresample.dylib",
    NULL
  };
#else
  static const char *const avutil_names[] = {
    "libavutil.so.61", "libavutil.so.60", "libavutil.so.59", "libavutil.so.58", "libavutil.so", NULL
  };
  static const char *const avcodec_names[] = {
    "libavcodec.so.63", "libavcodec.so.62", "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so", NULL
  };
  static const char *const avformat_names[] = {
    "libavformat.so.63", "libavformat.so.62", "libavformat.so.61", "libavformat.so.60", "libavformat.so", NULL
  };
  static const char *const swscale_names[] = {
    "libswscale.so.10", "libswscale.so.9", "libswscale.so.8", "libswscale.so.7", "libswscale.so", NULL
  };
  static const char *const swresample_names[] = {
    "libswresample.so.7", "libswresample.so.6", "libswresample.so.5", "libswresample.so.4", "libswresample.so", NULL
  };
#endif

  g_av.h_avutil = try_dlopen_list(avutil_names);
  g_av.h_avcodec = try_dlopen_list(avcodec_names);
  g_av.h_avformat = try_dlopen_list(avformat_names);
  g_av.h_swscale = try_dlopen_list(swscale_names);
  g_av.h_swresample = try_dlopen_list(swresample_names);

  if (!g_av.h_avutil || !g_av.h_avcodec || !g_av.h_avformat || !g_av.h_swscale || !g_av.h_swresample) {
    return;
  }

  // libavutil
  LOAD_SYM(g_av.h_avutil, avutil_version);
  LOAD_SYM(g_av.h_avutil, av_malloc);
  LOAD_SYM(g_av.h_avutil, av_free);
  LOAD_SYM(g_av.h_avutil, av_freep);
  LOAD_SYM(g_av.h_avutil, av_log_set_level);
  LOAD_SYM(g_av.h_avutil, av_frame_alloc);
  LOAD_SYM(g_av.h_avutil, av_frame_free);
  LOAD_SYM(g_av.h_avutil, av_frame_unref);
  LOAD_SYM(g_av.h_avutil, av_frame_get_buffer);
  LOAD_SYM(g_av.h_avutil, av_get_pix_fmt);
  LOAD_SYM(g_av.h_avutil, av_get_sample_fmt);
  LOAD_OPT_SYM(g_av.h_avutil, av_hwdevice_find_type_by_name);
  LOAD_OPT_SYM(g_av.h_avutil, av_hwdevice_ctx_create);
  LOAD_OPT_SYM(g_av.h_avutil, av_hwframe_transfer_data);
  LOAD_OPT_SYM(g_av.h_avutil, av_buffer_ref);
  LOAD_OPT_SYM(g_av.h_avutil, av_buffer_unref);
  LOAD_SYM(g_av.h_avutil, av_rescale_q);
  LOAD_SYM(g_av.h_avutil, av_channel_layout_default);
  LOAD_SYM(g_av.h_avutil, av_channel_layout_uninit);
  LOAD_SYM(g_av.h_avutil, av_channel_layout_copy);
  LOAD_SYM(g_av.h_avutil, av_opt_find);
  LOAD_SYM(g_av.h_avutil, av_opt_set_int);
  LOAD_SYM(g_av.h_avutil, av_dict_set);
  LOAD_SYM(g_av.h_avutil, av_dict_free);

  // libavcodec
  LOAD_SYM(g_av.h_avcodec, avcodec_version);
  LOAD_SYM(g_av.h_avcodec, avcodec_find_decoder);
  LOAD_SYM(g_av.h_avcodec, avcodec_find_encoder_by_name);
  LOAD_SYM(g_av.h_avcodec, avcodec_get_name);
  LOAD_SYM(g_av.h_avcodec, avcodec_alloc_context3);
  LOAD_SYM(g_av.h_avcodec, avcodec_free_context);
  LOAD_SYM(g_av.h_avcodec, avcodec_parameters_alloc);
  LOAD_SYM(g_av.h_avcodec, avcodec_parameters_free);
  LOAD_SYM(g_av.h_avcodec, avcodec_parameters_to_context);
  LOAD_SYM(g_av.h_avcodec, avcodec_open2);
  LOAD_SYM(g_av.h_avcodec, avcodec_flush_buffers);
  LOAD_SYM(g_av.h_avcodec, avcodec_send_packet);
  LOAD_SYM(g_av.h_avcodec, avcodec_receive_frame);
  LOAD_SYM(g_av.h_avcodec, avcodec_send_frame);
  LOAD_SYM(g_av.h_avcodec, avcodec_receive_packet);
  LOAD_SYM(g_av.h_avcodec, av_packet_alloc);
  LOAD_SYM(g_av.h_avcodec, av_packet_free);
  LOAD_SYM(g_av.h_avcodec, av_packet_unref);

  // libavformat
  LOAD_SYM(g_av.h_avformat, avformat_version);
  LOAD_SYM(g_av.h_avformat, avformat_alloc_context);
  LOAD_SYM(g_av.h_avformat, avformat_free_context);
  LOAD_SYM(g_av.h_avformat, avformat_open_input);
  LOAD_SYM(g_av.h_avformat, avformat_close_input);
  LOAD_SYM(g_av.h_avformat, avformat_find_stream_info);
  LOAD_SYM(g_av.h_avformat, av_find_best_stream);
  LOAD_SYM(g_av.h_avformat, av_read_frame);
  LOAD_SYM(g_av.h_avformat, av_seek_frame);
  LOAD_SYM(g_av.h_avformat, avio_alloc_context);
  LOAD_SYM(g_av.h_avformat, avio_context_free);

  // libswscale
  LOAD_SYM(g_av.h_swscale, sws_getContext);
  LOAD_SYM(g_av.h_swscale, sws_scale);
  LOAD_SYM(g_av.h_swscale, sws_freeContext);

  // libswresample
  LOAD_SYM(g_av.h_swresample, swr_alloc_set_opts2);
  LOAD_SYM(g_av.h_swresample, swr_init);
  LOAD_SYM(g_av.h_swresample, swr_convert);
  LOAD_SYM(g_av.h_swresample, swr_get_delay);
  LOAD_SYM(g_av.h_swresample, swr_free);

  g_av.avf_major = (int)(g_av.avformat_version() >> 16);
  g_av.avc_major = (int)(g_av.avcodec_version() >> 16);
  g_av.avu_major = (int)(g_av.avutil_version() >> 16);

  if (!calibrate_abi()) {
    return;
  }

  g_av.pix_fmt_rgba = g_av.av_get_pix_fmt("rgba");
  g_av.pix_fmt_yuv420p = g_av.av_get_pix_fmt("yuv420p");
  g_av.pix_fmt_yuvj420p = g_av.av_get_pix_fmt("yuvj420p");
  g_av.pix_fmt_nv12 = g_av.av_get_pix_fmt("nv12");
  g_av.pix_fmt_videotoolbox = g_av.av_get_pix_fmt("videotoolbox_vld");
  g_av.sample_fmt_s16 = g_av.av_get_sample_fmt("s16");
  g_av.sample_fmt_fltp = g_av.av_get_sample_fmt("fltp");
  g_av.hw_type_videotoolbox = g_av.av_hwdevice_find_type_by_name
    ? g_av.av_hwdevice_find_type_by_name("videotoolbox") : 0;

  if (g_av.pix_fmt_rgba < 0 || g_av.pix_fmt_yuv420p < 0 || g_av.sample_fmt_s16 < 0 || g_av.sample_fmt_fltp < 0) {
    return;
  }

  g_av.av_log_set_level(getenv("FV_MEDIA_LOG") ? FV_AV_LOG_ERROR : FV_AV_LOG_QUIET);
  g_av.available = 1;
}

int32_t fv_media_inproc_available(void) {
  pthread_once(&g_av_once, fv_av_do_init);
  return g_av.available ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Custom in-memory AVIOContext buffer (zero temp files, supports MSE append)
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  size_t pos;
  int owned;
} FvMemBuf;

static int membuf_append(FvMemBuf *m, const uint8_t *src, size_t n) {
  if (!m || !src || n == 0) return 0;
  if (n > FV_AV_MEM_MAX_BYTES || m->len > FV_AV_MEM_MAX_BYTES - n) return 0;
  if (m->len + n > m->cap) {
    size_t ncap = m->cap ? m->cap * 2 : 65536;
    while (ncap < m->len + n) ncap *= 2;
    if (ncap > FV_AV_MEM_MAX_BYTES) ncap = FV_AV_MEM_MAX_BYTES;
    uint8_t *nd = (uint8_t *)realloc(m->data, ncap);
    if (!nd) return 0;
    m->data = nd;
    m->cap = ncap;
  }
  memcpy(m->data + m->len, src, n);
  m->len += n;
  m->owned = 1;
  return 1;
}

static void membuf_free(FvMemBuf *m) {
  if (m && m->owned && m->data) {
    free(m->data);
  }
  if (m) {
    m->data = NULL;
    m->len = 0;
    m->cap = 0;
    m->pos = 0;
    m->owned = 0;
  }
}

static int avio_mem_read(void *opaque, uint8_t *buf, int buf_size) {
  FvMemBuf *m = (FvMemBuf *)opaque;
  if (buf_size <= 0 || !m || !m->data || m->pos >= m->len) return FV_AVERROR_EOF;
  size_t avail = m->len - m->pos;
  size_t n = (size_t)buf_size < avail ? (size_t)buf_size : avail;
  if (n == 0) return FV_AVERROR_EOF;
  memcpy(buf, m->data + m->pos, n);
  m->pos += n;
  return (int)n;
}

static int64_t avio_mem_seek(void *opaque, int64_t offset, int whence) {
  FvMemBuf *m = (FvMemBuf *)opaque;
  if (!m) return -1;
  if (whence == FV_AVSEEK_SIZE) return (int64_t)m->len;
  int64_t npos = 0;
  int w = whence & ~0x20000; // mask AVSEEK_FORCE if present
  if (w == SEEK_SET) npos = offset;
  else if (w == SEEK_CUR) npos = (int64_t)m->pos + offset;
  else if (w == SEEK_END) npos = (int64_t)m->len + offset;
  else return -1;
  if (npos < 0) return -1;
  if ((size_t)npos > m->len) npos = (int64_t)m->len;
  m->pos = (size_t)npos;
  return npos;
}

static int open_input_mem(FvMemBuf *m, void **out_ic, void **out_pb) {
  *out_ic = NULL;
  *out_pb = NULL;
  if (!m || !m->data || m->len == 0) return -1;
  m->pos = 0;

  void *ic = g_av.avformat_alloc_context();
  if (!ic) return -1;

  unsigned char *iobuf = (unsigned char *)g_av.av_malloc(FV_AV_IO_BUF_SIZE);
  if (!iobuf) {
    g_av.avformat_free_context(ic);
    return -1;
  }
  void *pb = g_av.avio_alloc_context(iobuf, FV_AV_IO_BUF_SIZE, 0, m, avio_mem_read, NULL, avio_mem_seek);
  if (!pb) {
    g_av.av_free(iobuf);
    g_av.avformat_free_context(ic);
    return -1;
  }
  wr_ptr(ic, g_av.abi.fc_pb, pb);

  int ret = g_av.avformat_open_input(&ic, NULL, NULL, NULL);
  if (ret < 0) {
    // On failure avformat_open_input frees ic (and sets ic=NULL), but NOT our custom pb.
    void *bufp = rd_ptr(pb, g_av.abi.io_buffer);
    if (bufp) g_av.av_freep((uint8_t *)pb + g_av.abi.io_buffer);
    g_av.avio_context_free(&pb);
    return ret;
  }
  ret = g_av.avformat_find_stream_info(ic, NULL);
  if (ret < 0) {
    g_av.avformat_close_input(&ic);
    void *bufp = rd_ptr(pb, g_av.abi.io_buffer);
    if (bufp) g_av.av_freep((uint8_t *)pb + g_av.abi.io_buffer);
    g_av.avio_context_free(&pb);
    return ret;
  }
  *out_ic = ic;
  *out_pb = pb;
  return 0;
}

static void close_input_mem(void **ic, void **pb) {
  if (ic && *ic) {
    g_av.avformat_close_input(ic);
  }
  if (pb && *pb) {
    void *bufp = rd_ptr(*pb, g_av.abi.io_buffer);
    if (bufp) g_av.av_freep((uint8_t *)(*pb) + g_av.abi.io_buffer);
    g_av.avio_context_free(pb);
  }
}

static int open_input_file(const char *path, void **out_ic) {
  *out_ic = NULL;
  if (!path || !path[0]) return -1;
  void *ic = NULL;
  int ret = g_av.avformat_open_input(&ic, path, NULL, NULL);
  if (ret < 0) return ret;
  ret = g_av.avformat_find_stream_info(ic, NULL);
  if (ret < 0) {
    g_av.avformat_close_input(&ic);
    return ret;
  }
  *out_ic = ic;
  return 0;
}

static int64_t pts_to_ms(int64_t pts, FvAVRational tb) {
  if (pts == FV_AV_NOPTS_VALUE || tb.den <= 0 || tb.num <= 0) return -1;
  FvAVRational ms_tb = { 1, 1000 };
  return g_av.av_rescale_q(pts, tb, ms_tb);
}

// ---------------------------------------------------------------------------
// In-process Probe (file or memory, with fMP4 packet-duration scan)
// ---------------------------------------------------------------------------

static moonbit_bytes_t probe_format_ctx(void *ic, int scan_packets_if_nodur) {
  const FvAvAbi *a = &g_av.abi;
  uint32_t nb_streams = rd_u32(ic, a->fc_nb_streams);
  void **streams = (void **)rd_ptr(ic, a->fc_streams);
  int64_t ic_dur = rd_i64(ic, a->fc_duration);
  double dur_sec = (ic_dur != FV_AV_NOPTS_VALUE && ic_dur > 0)
    ? ((double)ic_dur / (double)FV_AV_TIME_BASE) : -1.0;

  int vw = 0, vh = 0;
  int has_vid = 0, has_aud = 0;
  const char *vcodec = NULL;
  const char *acodec = NULL;
  int sample_rate = 0, channels = 0;
  double fps = 0.0;

  for (uint32_t i = 0; i < nb_streams && streams; i++) {
    void *st = streams[i];
    if (!st) continue;
    void *cp = rd_ptr(st, a->st_codecpar);
    if (!cp) continue;
    int ctype = rd_i32(cp, a->cp_codec_type);
    int cid = rd_i32(cp, a->cp_codec_id);
    FvAVRational tb = rd_rat(st, a->st_time_base);
    int64_t sdur = rd_i64(st, a->st_duration);
    if (sdur != FV_AV_NOPTS_VALUE && sdur > 0 && tb.den > 0) {
      double sd = (double)sdur * (double)tb.num / (double)tb.den;
      if (sd > dur_sec) dur_sec = sd;
    }
    if (ctype == FV_AVMEDIA_TYPE_VIDEO && !has_vid) {
      has_vid = 1;
      vw = rd_i32(cp, a->cp_width);
      vh = rd_i32(cp, a->cp_height);
      vcodec = g_av.avcodec_get_name(cid);
      FvAVRational fr = rd_rat(st, a->st_avg_frame_rate);
      if (fr.num > 0 && fr.den > 0) fps = (double)fr.num / (double)fr.den;
    } else if (ctype == FV_AVMEDIA_TYPE_AUDIO && !has_aud) {
      has_aud = 1;
      acodec = g_av.avcodec_get_name(cid);
      sample_rate = rd_i32(cp, a->cp_sample_rate);
      FvAVChannelLayout chl;
      memcpy(&chl, (const uint8_t *)cp + a->cp_ch_layout, sizeof(chl));
      channels = chl.nb_channels;
    }
  }

  int64_t buf_start_ms = -1;
  int64_t buf_end_ms = -1;
  if (scan_packets_if_nodur && (dur_sec <= 0.0 || nb_streams > 0)) {
    // Fast packet-only scan (no decoding) to compute exact buffered [start, end] for fMP4/MSE.
    void *pkt = g_av.av_packet_alloc();
    if (pkt) {
      int limit = 20000;
      while (limit-- > 0 && g_av.av_read_frame(ic, pkt) >= 0) {
        int sidx = rd_i32(pkt, a->pkt_stream_index);
        if (sidx >= 0 && (uint32_t)sidx < nb_streams && streams && streams[sidx]) {
          FvAVRational tb = rd_rat(streams[sidx], a->st_time_base);
          int64_t pts = rd_i64(pkt, a->pkt_pts);
          if (pts == FV_AV_NOPTS_VALUE) pts = rd_i64(pkt, a->pkt_dts);
          int64_t pdur = rd_i64(pkt, a->pkt_duration);
          int64_t t0 = pts_to_ms(pts, tb);
          if (t0 >= 0) {
            if (buf_start_ms < 0 || t0 < buf_start_ms) buf_start_ms = t0;
            int64_t t1 = t0;
            if (pdur > 0) {
              int64_t dms = pts_to_ms(pdur, tb);
              if (dms > 0) t1 += dms;
            }
            if (t1 > buf_end_ms) buf_end_ms = t1;
          }
        }
        g_av.av_packet_unref(pkt);
      }
      g_av.av_packet_free(&pkt);
    }
    if (buf_end_ms > 0 && (double)buf_end_ms / 1000.0 > dur_sec) {
      dur_sec = (double)buf_end_ms / 1000.0;
    }
  }
  if (buf_start_ms < 0 && dur_sec > 0.0) {
    buf_start_ms = 0;
    buf_end_ms = (int64_t)(dur_sec * 1000.0);
  }

  char out[1024];
  int off = 0;
  if (has_vid) {
    off += snprintf(out + off, sizeof(out) - (size_t)off,
                    "codec_type=video\nwidth=%d\nheight=%d\n", vw, vh);
    if (vcodec && vcodec[0]) {
      off += snprintf(out + off, sizeof(out) - (size_t)off, "video_codec=%s\n", vcodec);
    }
    if (fps > 0.0) {
      off += snprintf(out + off, sizeof(out) - (size_t)off, "fps=%.3f\n", fps);
    }
  }
  if (has_aud) {
    off += snprintf(out + off, sizeof(out) - (size_t)off, "codec_type=audio\n");
    if (acodec && acodec[0]) {
      off += snprintf(out + off, sizeof(out) - (size_t)off, "audio_codec=%s\n", acodec);
    }
    if (sample_rate > 0) {
      off += snprintf(out + off, sizeof(out) - (size_t)off, "sample_rate=%d\n", sample_rate);
    }
    if (channels > 0) {
      off += snprintf(out + off, sizeof(out) - (size_t)off, "channels=%d\n", channels);
    }
  }
  if (dur_sec > 0.0) {
    off += snprintf(out + off, sizeof(out) - (size_t)off, "duration=%.6f\n", dur_sec);
  }
  if (buf_start_ms >= 0 && buf_end_ms >= buf_start_ms) {
    off += snprintf(out + off, sizeof(out) - (size_t)off,
                    "buffered_start_ms=%lld\nbuffered_end_ms=%lld\n",
                    (long long)buf_start_ms, (long long)buf_end_ms);
  }
  if (off < 0) off = 0;
  if ((size_t)off >= sizeof(out)) off = (int)sizeof(out) - 1;
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)off, 0);
  if (off > 0) memcpy(res, out, (size_t)off);
  return res;
}

moonbit_bytes_t fv_av_probe_file(const char *path) {
  if (!fv_media_inproc_available() || !path || !path[0]) return moonbit_make_bytes(0, 0);
  void *ic = NULL;
  if (open_input_file(path, &ic) < 0) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t res = probe_format_ctx(ic, 0);
  g_av.avformat_close_input(&ic);
  return res;
}

moonbit_bytes_t fv_media_probe_mem(moonbit_bytes_t data_bytes) {
  if (!fv_media_inproc_available()) return moonbit_make_bytes(0, 0);
  size_t len = (size_t)Moonbit_array_length(data_bytes);
  if (len == 0) return moonbit_make_bytes(0, 0);
  FvMemBuf m = { (uint8_t *)data_bytes, len, len, 0, 0 };
  void *ic = NULL, *pb = NULL;
  if (open_input_mem(&m, &ic, &pb) < 0) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t res = probe_format_ctx(ic, 1);
  close_input_mem(&ic, &pb);
  return res;
}

// ---------------------------------------------------------------------------
// In-process Video Decoder Stream (with VideoToolbox HW accel + SW fallback)
// ---------------------------------------------------------------------------

typedef struct {
  int in_use;
  int is_mem;
  char path[512];
  FvMemBuf mem;
  int mem_ended;
  int need_reopen;

  void *ic;
  void *pb;
  void *dec_ctx;
  void *hw_device_ctx;
  int hw_active;

  void *sws_rgba;
  void *sws_yuv;
  void *jpeg_enc;
  int sws_src_w, sws_src_h, sws_src_fmt;
  int out_w, out_h;

  void *pkt;
  void *frame;
  void *sw_frame;
  void *yuv_frame;
  void *enc_pkt;

  int video_idx;
  FvAVRational time_base;
  int scale_w;
  int target_fps;
  int64_t last_pts_ms;
  int64_t min_next_pts_ms;
  int64_t pts_origin_ms;
  int64_t open_start_ms;
  int eof;
  int opened;
} AvVideoStream;

static AvVideoStream g_av_vstreams[FV_AV_MAX_STREAMS];

static void av_vstream_close_codec(AvVideoStream *s) {
  if (s->sws_rgba) { g_av.sws_freeContext(s->sws_rgba); s->sws_rgba = NULL; }
  if (s->sws_yuv) { g_av.sws_freeContext(s->sws_yuv); s->sws_yuv = NULL; }
  if (s->jpeg_enc) { g_av.avcodec_free_context(&s->jpeg_enc); }
  if (s->dec_ctx) { g_av.avcodec_free_context(&s->dec_ctx); }
  if (s->hw_device_ctx && g_av.av_buffer_unref) { g_av.av_buffer_unref(&s->hw_device_ctx); }
  if (s->pkt) { g_av.av_packet_free(&s->pkt); }
  if (s->enc_pkt) { g_av.av_packet_free(&s->enc_pkt); }
  if (s->frame) { g_av.av_frame_free(&s->frame); }
  if (s->sw_frame) { g_av.av_frame_free(&s->sw_frame); }
  if (s->yuv_frame) { g_av.av_frame_free(&s->yuv_frame); }
  if (s->is_mem) {
    close_input_mem(&s->ic, &s->pb);
  } else if (s->ic) {
    g_av.avformat_close_input(&s->ic);
  }
  s->hw_active = 0;
  s->sws_src_w = s->sws_src_h = 0;
  s->sws_src_fmt = -1;
  s->out_w = s->out_h = 0;
  s->video_idx = -1;
  s->opened = 0;
}

static void av_vstream_close_slot(int idx) {
  if (idx < 0 || idx >= FV_AV_MAX_STREAMS || !g_av_vstreams[idx].in_use) return;
  AvVideoStream *s = &g_av_vstreams[idx];
  av_vstream_close_codec(s);
  membuf_free(&s->mem);
  s->eof = 1;
  s->in_use = 0;
}

void fv_av_close_all_streams(void) {
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (g_av_vstreams[i].in_use) av_vstream_close_slot(i);
  }
}

static int open_video_decoder(AvVideoStream *s, int allow_hw) {
  const FvAvAbi *a = &g_av.abi;
  const void *dec = NULL;
  int vidx = g_av.av_find_best_stream(s->ic, FV_AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
  if (vidx < 0 || !dec) return -1;

  void **streams = (void **)rd_ptr(s->ic, a->fc_streams);
  void *st = streams[vidx];
  void *cp = rd_ptr(st, a->st_codecpar);

  void *cc = g_av.avcodec_alloc_context3(dec);
  if (!cc) return -1;
  if (g_av.avcodec_parameters_to_context(cc, cp) < 0) {
    g_av.avcodec_free_context(&cc);
    return -1;
  }
  FvAVRational tb = rd_rat(st, a->st_time_base);
  wr_rat(cc, a->cc_pkt_timebase, tb);
  wr_i32(cc, a->cc_thread_count, 2);

  // Attempt macOS VideoToolbox hardware acceleration only for codecs VideoToolbox supports
  // (avoids hwaccel setup error spam on VP9/VP8/AV1 WebM streams).
  s->hw_active = 0;
  int cid = rd_i32(cp, a->cp_codec_id);
  const char *cname = g_av.avcodec_get_name(cid);
  int vt_ok = cname && (strcmp(cname, "h264") == 0 || strcmp(cname, "hevc") == 0 ||
                        strcmp(cname, "mpeg4") == 0 || strcmp(cname, "prores") == 0);
  if (allow_hw && vt_ok && a->cc_hw_device_ctx > 0 && g_av.hw_type_videotoolbox > 0 &&
      g_av.pix_fmt_videotoolbox >= 0 && g_av.av_hwdevice_ctx_create &&
      g_av.av_buffer_ref && g_av.av_hwframe_transfer_data) {
    void *hw = NULL;
    if (g_av.av_hwdevice_ctx_create(&hw, g_av.hw_type_videotoolbox, NULL, NULL, 0) == 0 && hw) {
      void *ref = g_av.av_buffer_ref(hw);
      if (ref) {
        wr_ptr(cc, a->cc_hw_device_ctx, ref);
        s->hw_device_ctx = hw;
        s->hw_active = 1;
      } else {
        g_av.av_buffer_unref(&hw);
      }
    }
  }

  if (g_av.avcodec_open2(cc, dec, NULL) < 0) {
    // Retry pure software if HW init failed.
    if (s->hw_active) {
      g_av.avcodec_free_context(&cc);
      if (s->hw_device_ctx && g_av.av_buffer_unref) g_av.av_buffer_unref(&s->hw_device_ctx);
      s->hw_active = 0;
      return open_video_decoder(s, 0);
    }
    g_av.avcodec_free_context(&cc);
    return -1;
  }

  s->dec_ctx = cc;
  s->video_idx = vidx;
  s->time_base = tb;
  s->pkt = g_av.av_packet_alloc();
  s->enc_pkt = g_av.av_packet_alloc();
  s->frame = g_av.av_frame_alloc();
  s->sw_frame = g_av.av_frame_alloc();
  s->yuv_frame = g_av.av_frame_alloc();
  s->sws_src_fmt = -1;
  s->opened = 1;
  return 0;
}

static int av_vstream_open_internal(AvVideoStream *s, int32_t start_ms) {
  av_vstream_close_codec(s);
  int ret = s->is_mem ? open_input_mem(&s->mem, &s->ic, &s->pb)
                      : open_input_file(s->path, &s->ic);
  if (ret < 0) return ret;
  if (open_video_decoder(s, 1) < 0) {
    av_vstream_close_codec(s);
    return -1;
  }
  s->eof = 0;
  s->need_reopen = 0;
  s->open_start_ms = start_ms > 0 ? (int64_t)start_ms : 0;
  if (start_ms > 0) {
    int64_t seek_ms = (int64_t)start_ms + (s->pts_origin_ms > 0 ? s->pts_origin_ms : 0);
    int64_t target_ts = g_av.av_rescale_q(seek_ms, (FvAVRational){1, 1000}, s->time_base);
    g_av.av_seek_frame(s->ic, s->video_idx, target_ts, FV_AVSEEK_FLAG_BACKWARD);
    g_av.avcodec_flush_buffers(s->dec_ctx);
    s->min_next_pts_ms = start_ms;
  } else {
    s->min_next_pts_ms = -1;
  }
  return 0;
}

static int ensure_jpeg_encoder(AvVideoStream *s, int ow, int oh) {
  const FvAvAbi *a = &g_av.abi;
  if (s->jpeg_enc && s->out_w == ow && s->out_h == oh) return 0;
  if (s->jpeg_enc) g_av.avcodec_free_context(&s->jpeg_enc);
  if (s->yuv_frame) g_av.av_frame_unref(s->yuv_frame);

  const void *enc = g_av.avcodec_find_encoder_by_name("mjpeg");
  if (!enc) return -1;
  void *ec = g_av.avcodec_alloc_context3(enc);
  if (!ec) return -1;

  wr_i32(ec, a->cc_width, ow);
  wr_i32(ec, a->cc_height, oh);
  int pix = g_av.pix_fmt_yuvj420p >= 0 ? g_av.pix_fmt_yuvj420p : g_av.pix_fmt_yuv420p;
  wr_i32(ec, a->cc_pix_fmt, pix);
  wr_rat(ec, a->cc_time_base, (FvAVRational){1, 25});
  g_av.av_opt_set_int(ec, "qmin", 4, 0);
  g_av.av_opt_set_int(ec, "qmax", 7, 0);

  void *opts = NULL;
  g_av.av_dict_set(&opts, "strict", "unofficial", 0);
  int ret = g_av.avcodec_open2(ec, enc, &opts);
  g_av.av_dict_free(&opts);
  if (ret < 0) {
    g_av.avcodec_free_context(&ec);
    return -1;
  }

  wr_i32(s->yuv_frame, a->fr_width, ow);
  wr_i32(s->yuv_frame, a->fr_height, oh);
  wr_i32(s->yuv_frame, a->fr_format, pix);
  if (g_av.av_frame_get_buffer(s->yuv_frame, 32) < 0) {
    g_av.avcodec_free_context(&ec);
    return -1;
  }

  s->jpeg_enc = ec;
  s->out_w = ow;
  s->out_h = oh;
  return 0;
}

static void compute_out_dims(int sw, int sh, int scale_w, int *ow, int *oh) {
  int w = (scale_w > 0 && sw > scale_w) ? scale_w : sw;
  if (w < 2) w = 2;
  w &= ~1;
  int h = (int)((int64_t)sh * w / (sw > 0 ? sw : 1));
  if (h < 2) h = 2;
  h &= ~1;
  *ow = w;
  *oh = h;
}

// Decodes the next software-accessible AVFrame (transferring from VideoToolbox if needed),
// honoring min_next_pts_ms for seek/fps pacing. Returns pointer to frame (s->frame or s->sw_frame),
// or NULL on EOF/error.
static void *decode_next_video_frame(AvVideoStream *s, int64_t *out_pts_ms) {
  const FvAvAbi *a = &g_av.abi;
  if (s->is_mem && (!s->opened || s->need_reopen)) {
    if (s->mem.len == 0) return NULL;
    int32_t resume = (s->need_reopen && s->last_pts_ms >= 0) ? (int32_t)(s->last_pts_ms + 1) : 0;
    if (av_vstream_open_internal(s, resume) < 0) return NULL;
  }
  if (!s->opened || s->eof) return NULL;

  int max_skip = 300;
  while (max_skip-- > 0) {
    int ret = g_av.avcodec_receive_frame(s->dec_ctx, s->frame);
    if (ret == 0) {
      int64_t pts = rd_i64(s->frame, a->fr_pts);
      int64_t raw_pms = pts_to_ms(pts, s->time_base);
      int64_t pms;
      if (raw_pms >= 0) {
        if (s->pts_origin_ms < 0) {
          if (s->open_start_ms == 0) {
            s->pts_origin_ms = raw_pms;
          } else if (raw_pms > s->open_start_ms + 500) {
            s->pts_origin_ms = raw_pms - s->open_start_ms;
          } else {
            s->pts_origin_ms = 0;
          }
        }
        pms = raw_pms - s->pts_origin_ms;
        if (pms < 0) pms = 0;
      } else {
        pms = (s->last_pts_ms >= 0) ? (s->last_pts_ms + (1000 / (s->target_fps > 0 ? s->target_fps : 24))) : 0;
      }

      if (s->min_next_pts_ms >= 0 && pms < s->min_next_pts_ms && max_skip > 1) {
        g_av.av_frame_unref(s->frame);
        continue;
      }

      void *usable = s->frame;
      int fmt = rd_i32(s->frame, a->fr_format);
      if (s->hw_active && fmt == g_av.pix_fmt_videotoolbox && g_av.av_hwframe_transfer_data) {
        g_av.av_frame_unref(s->sw_frame);
        if (g_av.av_hwframe_transfer_data(s->sw_frame, s->frame, 0) == 0) {
          usable = s->sw_frame;
        } else {
          // HW transfer failed: transparently reopen in SW mode.
          g_av.av_frame_unref(s->frame);
          int64_t cur = pms;
          av_vstream_close_codec(s);
          int r2 = s->is_mem ? open_input_mem(&s->mem, &s->ic, &s->pb)
                             : open_input_file(s->path, &s->ic);
          if (r2 < 0 || open_video_decoder(s, 0) < 0) {
            s->eof = 1;
            return NULL;
          }
          if (cur > 0) {
            int64_t seek_ms = cur + (s->pts_origin_ms > 0 ? s->pts_origin_ms : 0);
            int64_t ts = g_av.av_rescale_q(seek_ms, (FvAVRational){1, 1000}, s->time_base);
            g_av.av_seek_frame(s->ic, s->video_idx, ts, FV_AVSEEK_FLAG_BACKWARD);
            g_av.avcodec_flush_buffers(s->dec_ctx);
            s->min_next_pts_ms = cur;
          }
          continue;
        }
      }

      s->last_pts_ms = pms;
      if (s->target_fps > 0) {
        s->min_next_pts_ms = pms + (1000 / s->target_fps) - 2;
      } else {
        s->min_next_pts_ms = -1;
      }
      if (out_pts_ms) *out_pts_ms = pms;
      return usable;
    }

    if (ret == FV_AVERROR_EOF) {
      s->eof = 1;
      return NULL;
    }
    if (ret != FV_AVERROR_EAGAIN && ret < 0) {
      s->eof = 1;
      return NULL;
    }

    // Feed next video packet
    int got_pkt = 0;
    while (g_av.av_read_frame(s->ic, s->pkt) >= 0) {
      if (rd_i32(s->pkt, a->pkt_stream_index) == s->video_idx) {
        g_av.avcodec_send_packet(s->dec_ctx, s->pkt);
        g_av.av_packet_unref(s->pkt);
        got_pkt = 1;
        break;
      }
      g_av.av_packet_unref(s->pkt);
    }
    if (!got_pkt) {
      // Drain decoder
      g_av.avcodec_send_packet(s->dec_ctx, NULL);
    }
  }
  return NULL;
}

static moonbit_bytes_t encode_frame_to_jpeg(AvVideoStream *s, void *src_frame) {
  const FvAvAbi *a = &g_av.abi;
  int sw = rd_i32(src_frame, a->fr_width);
  int sh = rd_i32(src_frame, a->fr_height);
  int sfmt = rd_i32(src_frame, a->fr_format);
  if (sw <= 0 || sh <= 0 || sfmt < 0) return moonbit_make_bytes(0, 0);

  int ow = 0, oh = 0;
  compute_out_dims(sw, sh, s->scale_w, &ow, &oh);
  if (ensure_jpeg_encoder(s, ow, oh) < 0) return moonbit_make_bytes(0, 0);

  int dst_pix = g_av.pix_fmt_yuvj420p >= 0 ? g_av.pix_fmt_yuvj420p : g_av.pix_fmt_yuv420p;
  if (!s->sws_yuv || s->sws_src_w != sw || s->sws_src_h != sh || s->sws_src_fmt != sfmt) {
    if (s->sws_yuv) g_av.sws_freeContext(s->sws_yuv);
    s->sws_yuv = g_av.sws_getContext(sw, sh, sfmt, ow, oh, dst_pix, FV_SWS_BILINEAR, NULL, NULL, NULL);
    s->sws_src_w = sw;
    s->sws_src_h = sh;
    s->sws_src_fmt = sfmt;
  }
  if (!s->sws_yuv) return moonbit_make_bytes(0, 0);

  const uint8_t *const *src_data = (const uint8_t *const *)((const uint8_t *)src_frame + a->fr_data);
  const int *src_ls = (const int *)((const uint8_t *)src_frame + a->fr_linesize);
  uint8_t *const *dst_data = (uint8_t *const *)((uint8_t *)s->yuv_frame + a->fr_data);
  const int *dst_ls = (const int *)((const uint8_t *)s->yuv_frame + a->fr_linesize);

  g_av.sws_scale(s->sws_yuv, src_data, src_ls, 0, sh, dst_data, dst_ls);

  static int64_t g_enc_pts = 0;
  wr_i64(s->yuv_frame, a->fr_pts, g_enc_pts++);

  if (g_av.avcodec_send_frame(s->jpeg_enc, s->yuv_frame) < 0) return moonbit_make_bytes(0, 0);
  if (g_av.avcodec_receive_packet(s->jpeg_enc, s->enc_pkt) < 0) return moonbit_make_bytes(0, 0);

  uint8_t *pdata = (uint8_t *)rd_ptr(s->enc_pkt, a->pkt_data);
  int psize = rd_i32(s->enc_pkt, a->pkt_size);
  moonbit_bytes_t res = moonbit_make_bytes(psize > 0 ? psize : 0, 0);
  if (psize > 0 && pdata) memcpy(res, pdata, (size_t)psize);
  g_av.av_packet_unref(s->enc_pkt);
  return res;
}

static moonbit_bytes_t convert_frame_to_rgba_pkt(AvVideoStream *s, void *src_frame, int64_t pts_ms) {
  const FvAvAbi *a = &g_av.abi;
  int sw = rd_i32(src_frame, a->fr_width);
  int sh = rd_i32(src_frame, a->fr_height);
  int sfmt = rd_i32(src_frame, a->fr_format);
  if (sw <= 0 || sh <= 0 || sfmt < 0) return moonbit_make_bytes(0, 0);

  int ow = 0, oh = 0;
  compute_out_dims(sw, sh, s->scale_w, &ow, &oh);
  if (!s->sws_rgba || s->sws_src_w != sw || s->sws_src_h != sh || s->sws_src_fmt != sfmt || s->out_w != ow || s->out_h != oh) {
    if (s->sws_rgba) g_av.sws_freeContext(s->sws_rgba);
    s->sws_rgba = g_av.sws_getContext(sw, sh, sfmt, ow, oh, g_av.pix_fmt_rgba, FV_SWS_BILINEAR, NULL, NULL, NULL);
    s->sws_src_w = sw;
    s->sws_src_h = sh;
    s->sws_src_fmt = sfmt;
    s->out_w = ow;
    s->out_h = oh;
  }
  if (!s->sws_rgba) return moonbit_make_bytes(0, 0);

  size_t px_bytes = (size_t)ow * (size_t)oh * 4;
  size_t total = 16 + px_bytes;
  if (total > (size_t)INT32_MAX) return moonbit_make_bytes(0, 0);

  moonbit_bytes_t res = moonbit_make_bytes((int32_t)total, 0);
  uint32_t uw = (uint32_t)ow, uh = (uint32_t)oh;
  memcpy(res + 0, &uw, 4);
  memcpy(res + 4, &uh, 4);
  memcpy(res + 8, &pts_ms, 8);

  const uint8_t *const *src_data = (const uint8_t *const *)((const uint8_t *)src_frame + a->fr_data);
  const int *src_ls = (const int *)((const uint8_t *)src_frame + a->fr_linesize);
  uint8_t *dst_data[4] = { (uint8_t *)res + 16, NULL, NULL, NULL };
  int dst_ls[4] = { ow * 4, 0, 0, 0 };
  g_av.sws_scale(s->sws_rgba, src_data, src_ls, 0, sh, dst_data, dst_ls);
  return res;
}

moonbit_bytes_t fv_av_frame_jpeg_file(const char *path, int32_t time_ms, int32_t scale_w) {
  if (!fv_media_inproc_available() || !path || !path[0]) return moonbit_make_bytes(0, 0);
  AvVideoStream tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.is_mem = 0;
  snprintf(tmp.path, sizeof(tmp.path), "%s", path);
  tmp.scale_w = scale_w > 0 ? scale_w : 640;
  tmp.target_fps = 0;
  tmp.last_pts_ms = -1;
  if (av_vstream_open_internal(&tmp, time_ms > 0 ? time_ms : 0) < 0) return moonbit_make_bytes(0, 0);
  int64_t pts = 0;
  void *fr = decode_next_video_frame(&tmp, &pts);
  moonbit_bytes_t res = fr ? encode_frame_to_jpeg(&tmp, fr) : moonbit_make_bytes(0, 0);
  if (fr) g_av.av_frame_unref(tmp.frame);
  av_vstream_close_codec(&tmp);
  return res;
}

moonbit_bytes_t fv_media_frame_jpeg_mem(moonbit_bytes_t data_bytes, int32_t time_ms, int32_t scale_w) {
  if (!fv_media_inproc_available()) return moonbit_make_bytes(0, 0);
  size_t len = (size_t)Moonbit_array_length(data_bytes);
  if (len == 0) return moonbit_make_bytes(0, 0);
  AvVideoStream tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.is_mem = 1;
  tmp.mem.data = (uint8_t *)data_bytes;
  tmp.mem.len = len;
  tmp.mem.cap = len;
  tmp.mem.owned = 0;
  tmp.scale_w = scale_w > 0 ? scale_w : 640;
  tmp.target_fps = 0;
  tmp.last_pts_ms = -1;
  if (av_vstream_open_internal(&tmp, time_ms > 0 ? time_ms : 0) < 0) return moonbit_make_bytes(0, 0);
  int64_t pts = 0;
  void *fr = decode_next_video_frame(&tmp, &pts);
  moonbit_bytes_t res = fr ? encode_frame_to_jpeg(&tmp, fr) : moonbit_make_bytes(0, 0);
  if (fr) g_av.av_frame_unref(tmp.frame);
  av_vstream_close_codec(&tmp);
  return res;
}

static int alloc_vstream_slot(void) {
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (!g_av_vstreams[i].in_use) {
      memset(&g_av_vstreams[i], 0, sizeof(AvVideoStream));
      g_av_vstreams[i].in_use = 1;
      g_av_vstreams[i].video_idx = -1;
      g_av_vstreams[i].sws_src_fmt = -1;
      g_av_vstreams[i].last_pts_ms = -1;
      g_av_vstreams[i].min_next_pts_ms = -1;
      g_av_vstreams[i].pts_origin_ms = -1;
      g_av_vstreams[i].open_start_ms = 0;
      return i;
    }
  }
  return -1;
}

int32_t fv_av_stream_open_file(const char *path, int32_t start_ms, int32_t scale_w, int32_t fps) {
  if (!fv_media_inproc_available() || !path || !path[0]) return -1;
  int slot = alloc_vstream_slot();
  if (slot < 0) return -1;
  AvVideoStream *s = &g_av_vstreams[slot];
  s->is_mem = 0;
  snprintf(s->path, sizeof(s->path), "%s", path);
  s->scale_w = scale_w > 0 ? scale_w : 640;
  s->target_fps = fps > 0 ? fps : 24;
  if (av_vstream_open_internal(s, start_ms > 0 ? start_ms : 0) < 0) {
    av_vstream_close_slot(slot);
    return -1;
  }
  return FV_AV_HANDLE_BASE + slot;
}

int32_t fv_media_mem_stream_open(int32_t scale_w, int32_t fps) {
  if (!fv_media_inproc_available()) return -1;
  int slot = alloc_vstream_slot();
  if (slot < 0) return -1;
  AvVideoStream *s = &g_av_vstreams[slot];
  s->is_mem = 1;
  s->scale_w = scale_w > 0 ? scale_w : 640;
  s->target_fps = fps > 0 ? fps : 24;
  s->eof = 0;
  s->opened = 0;
  return FV_AV_HANDLE_BASE + slot;
}

static AvVideoStream *get_vstream(int32_t handle) {
  int idx = handle - FV_AV_HANDLE_BASE;
  if (idx < 0 || idx >= FV_AV_MAX_STREAMS || !g_av_vstreams[idx].in_use) return NULL;
  return &g_av_vstreams[idx];
}

static int membuf_has_webm_cluster(const FvMemBuf *m) {
  if (!m || !m->data || m->len < 4) return 0;
  for (size_t i = 0; i + 4 <= m->len; i++) {
    if (m->data[i] == 0x1F && m->data[i + 1] == 0x43 &&
        m->data[i + 2] == 0xB6 && m->data[i + 3] == 0x75) {
      return 1;
    }
  }
  return 0;
}

static int membuf_append_mse(FvMemBuf *m, const uint8_t *src, size_t n) {
  if (!m || !src || n == 0) return 0;
  int is_ebml = (n >= 4 && src[0] == 0x1A && src[1] == 0x45 && src[2] == 0xDF && src[3] == 0xA3);
  int is_ftyp = (n >= 8 && src[4] == 'f' && src[5] == 't' && src[6] == 'y' && src[7] == 'p');
  if (m->len > 0 && (is_ebml || is_ftyp)) {
    // Identical re-appended init segment (common in YouTube/HLS MSE): skip duplicating it.
    if (n <= m->len && memcmp(m->data, src, n) == 0) {
      return 1;
    }
    // New WebM init segment arriving before any Cluster was appended: replace stale init header.
    if (is_ebml && !membuf_has_webm_cluster(m)) {
      m->len = 0;
      m->pos = 0;
    }
  }
  return membuf_append(m, src, n);
}

int32_t fv_media_mem_stream_append(int32_t handle, moonbit_bytes_t data_bytes) {
  AvVideoStream *s = get_vstream(handle);
  if (!s || !s->is_mem) return 0;
  size_t len = (size_t)Moonbit_array_length(data_bytes);
  if (len == 0) return 0;
  size_t old_len = s->mem.len;
  if (!membuf_append_mse(&s->mem, (const uint8_t *)data_bytes, len)) return 0;
  if (s->opened && (s->eof || s->last_pts_ms < 0 || s->mem.pos >= old_len)) {
    s->eof = 0;
    s->need_reopen = 1;
  }
  return 1;
}

void fv_media_mem_stream_end(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s || !s->is_mem) return;
  s->mem_ended = 1;
}

moonbit_bytes_t fv_media_mem_stream_probe(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s || !s->is_mem || s->mem.len == 0) return moonbit_make_bytes(0, 0);
  FvMemBuf tmp = { s->mem.data, s->mem.len, s->mem.len, 0, 0 };
  void *ic = NULL, *pb = NULL;
  if (open_input_mem(&tmp, &ic, &pb) < 0) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t res = probe_format_ctx(ic, 1);
  close_input_mem(&ic, &pb);
  return res;
}

moonbit_bytes_t fv_av_stream_read_jpeg(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s) return moonbit_make_bytes(0, 0);
  int64_t pts = 0;
  void *fr = decode_next_video_frame(s, &pts);
  if (!fr) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t res = encode_frame_to_jpeg(s, fr);
  g_av.av_frame_unref(s->frame);
  return res;
}

moonbit_bytes_t fv_media_stream_read_rgba(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s) return moonbit_make_bytes(0, 0);
  int64_t pts = 0;
  void *fr = decode_next_video_frame(s, &pts);
  if (!fr) return moonbit_make_bytes(0, 0);
  moonbit_bytes_t res = convert_frame_to_rgba_pkt(s, fr, pts);
  g_av.av_frame_unref(s->frame);
  return res;
}

int32_t fv_media_stream_last_pts_ms(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s || s->last_pts_ms < 0) return -1;
  return (int32_t)s->last_pts_ms;
}

int32_t fv_media_stream_seek(int32_t handle, int32_t target_ms) {
  AvVideoStream *s = get_vstream(handle);
  if (!s) return 0;
  if (target_ms < 0) target_ms = 0;
  if (!s->opened || s->need_reopen || s->eof) {
    if (av_vstream_open_internal(s, target_ms) < 0) return 0;
    return 1;
  }
  int64_t seek_ms = (int64_t)target_ms + (s->pts_origin_ms > 0 ? s->pts_origin_ms : 0);
  int64_t ts = g_av.av_rescale_q(seek_ms, (FvAVRational){1, 1000}, s->time_base);
  if (g_av.av_seek_frame(s->ic, s->video_idx, ts, FV_AVSEEK_FLAG_BACKWARD) < 0) {
    // Fallback: reopen at target_ms
    return av_vstream_open_internal(s, target_ms) == 0 ? 1 : 0;
  }
  g_av.avcodec_flush_buffers(s->dec_ctx);
  s->eof = 0;
  s->min_next_pts_ms = target_ms;
  return 1;
}

int32_t fv_av_stream_eof(int32_t handle) {
  AvVideoStream *s = get_vstream(handle);
  if (!s) return 1;
  if (s->is_mem && !s->mem_ended) return 0;
  return s->eof ? 1 : 0;
}

void fv_av_stream_close(int32_t handle) {
  av_vstream_close_slot(handle - FV_AV_HANDLE_BASE);
}

int32_t fv_av_stream_count(void) {
  int n = 0;
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (g_av_vstreams[i].in_use) n++;
  }
  return n;
}

// ---------------------------------------------------------------------------
// In-process Audio Decoder Stream (48 kHz Stereo S16LE PCM + exact PTS ms)
// ---------------------------------------------------------------------------

typedef struct {
  int in_use;
  int is_mem;
  char path[512];
  FvMemBuf mem;
  int mem_ended;
  int need_reopen;

  void *ic;
  void *pb;
  void *dec_ctx;
  void *swr;
  void *pkt;
  void *frame;

  int audio_idx;
  FvAVRational time_base;
  int64_t last_pts_ms;
  int64_t min_pts_ms;
  int eof;
  int opened;

  uint8_t *pcm_buf;
  size_t pcm_len;
  size_t pcm_cap;
  int32_t out_handle; /* optional fv_audio_out handle for real-time speaker playback */
} AvAudioStream;

static AvAudioStream g_av_astreams[FV_AV_MAX_STREAMS];

static void av_astream_close_codec(AvAudioStream *s) {
  if (s->swr) { g_av.swr_free(&s->swr); }
  if (s->dec_ctx) { g_av.avcodec_free_context(&s->dec_ctx); }
  if (s->pkt) { g_av.av_packet_free(&s->pkt); }
  if (s->frame) { g_av.av_frame_free(&s->frame); }
  if (s->is_mem) {
    close_input_mem(&s->ic, &s->pb);
  } else if (s->ic) {
    g_av.avformat_close_input(&s->ic);
  }
  s->audio_idx = -1;
  s->opened = 0;
}

static void av_astream_close_slot(int idx) {
  if (idx < 0 || idx >= FV_AV_MAX_STREAMS || !g_av_astreams[idx].in_use) return;
  AvAudioStream *s = &g_av_astreams[idx];
  av_astream_close_codec(s);
  membuf_free(&s->mem);
  if (s->pcm_buf) { free(s->pcm_buf); s->pcm_buf = NULL; }
  s->pcm_len = s->pcm_cap = 0;
  s->eof = 1;
  s->in_use = 0;
}

void fv_av_close_all_audio(void) {
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (g_av_astreams[i].in_use) av_astream_close_slot(i);
  }
  fv_audio_out_close_all();
}

static int av_astream_open_internal(AvAudioStream *s, int32_t start_ms) {
  const FvAvAbi *a = &g_av.abi;
  av_astream_close_codec(s);
  int ret = s->is_mem ? open_input_mem(&s->mem, &s->ic, &s->pb)
                      : open_input_file(s->path, &s->ic);
  if (ret < 0) return ret;

  const void *dec = NULL;
  int aidx = g_av.av_find_best_stream(s->ic, FV_AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if (aidx < 0 || !dec) {
    av_astream_close_codec(s);
    return -1;
  }

  void **streams = (void **)rd_ptr(s->ic, a->fc_streams);
  void *st = streams[aidx];
  void *cp = rd_ptr(st, a->st_codecpar);

  void *cc = g_av.avcodec_alloc_context3(dec);
  if (!cc) { av_astream_close_codec(s); return -1; }
  if (g_av.avcodec_parameters_to_context(cc, cp) < 0 || g_av.avcodec_open2(cc, dec, NULL) < 0) {
    g_av.avcodec_free_context(&cc);
    av_astream_close_codec(s);
    return -1;
  }

  int in_sr = rd_i32(cc, a->cc_sample_rate);
  int in_fmt = rd_i32(cc, a->cc_sample_fmt);
  FvAVChannelLayout in_ch;
  memcpy(&in_ch, (const uint8_t *)cc + a->cc_ch_layout, sizeof(in_ch));
  if (in_sr <= 0) in_sr = 48000;
  FvAVChannelLayout fallback_in_ch;
  memset(&fallback_in_ch, 0, sizeof(fallback_in_ch));
  if (in_ch.nb_channels <= 0) {
    g_av.av_channel_layout_default(&fallback_in_ch, 2);
    in_ch = fallback_in_ch;
  }

  FvAVChannelLayout out_ch;
  memset(&out_ch, 0, sizeof(out_ch));
  g_av.av_channel_layout_default(&out_ch, 2);

  void *swr = NULL;
  ret = g_av.swr_alloc_set_opts2(&swr, &out_ch, g_av.sample_fmt_s16, 48000,
                                 &in_ch, in_fmt, in_sr, 0, NULL);
  g_av.av_channel_layout_uninit(&out_ch);
  if (fallback_in_ch.nb_channels > 0) g_av.av_channel_layout_uninit(&fallback_in_ch);
  if (ret < 0 || !swr || g_av.swr_init(swr) < 0) {
    if (swr) g_av.swr_free(&swr);
    g_av.avcodec_free_context(&cc);
    av_astream_close_codec(s);
    return -1;
  }

  s->dec_ctx = cc;
  s->swr = swr;
  s->audio_idx = aidx;
  s->time_base = rd_rat(st, a->st_time_base);
  s->pkt = g_av.av_packet_alloc();
  s->frame = g_av.av_frame_alloc();
  s->eof = 0;
  s->need_reopen = 0;
  s->opened = 1;
  s->pcm_len = 0;

  if (start_ms > 0) {
    int64_t ts = g_av.av_rescale_q((int64_t)start_ms, (FvAVRational){1, 1000}, s->time_base);
    g_av.av_seek_frame(s->ic, s->audio_idx, ts, FV_AVSEEK_FLAG_BACKWARD);
    g_av.avcodec_flush_buffers(s->dec_ctx);
    s->min_pts_ms = start_ms;
  } else {
    s->min_pts_ms = -1;
  }
  return 0;
}

static int alloc_astream_slot(void) {
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (!g_av_astreams[i].in_use) {
      memset(&g_av_astreams[i], 0, sizeof(AvAudioStream));
      g_av_astreams[i].in_use = 1;
      g_av_astreams[i].audio_idx = -1;
      g_av_astreams[i].last_pts_ms = -1;
      g_av_astreams[i].min_pts_ms = -1;
      return i;
    }
  }
  return -1;
}

static AvAudioStream *get_astream(int32_t handle) {
  int idx = handle - FV_AV_HANDLE_BASE;
  if (idx < 0 || idx >= FV_AV_MAX_STREAMS || !g_av_astreams[idx].in_use) return NULL;
  return &g_av_astreams[idx];
}

int32_t fv_av_audio_open_file(const char *path, int32_t start_ms) {
  if (!fv_media_inproc_available() || !path || !path[0]) return -1;
  int slot = alloc_astream_slot();
  if (slot < 0) return -1;
  AvAudioStream *s = &g_av_astreams[slot];
  s->is_mem = 0;
  snprintf(s->path, sizeof(s->path), "%s", path);
  if (av_astream_open_internal(s, start_ms > 0 ? start_ms : 0) < 0) {
    av_astream_close_slot(slot);
    return -1;
  }
  return FV_AV_HANDLE_BASE + slot;
}

int32_t fv_media_mem_audio_open(void) {
  if (!fv_media_inproc_available()) return -1;
  int slot = alloc_astream_slot();
  if (slot < 0) return -1;
  AvAudioStream *s = &g_av_astreams[slot];
  s->is_mem = 1;
  s->eof = 0;
  s->opened = 0;
  return FV_AV_HANDLE_BASE + slot;
}

int32_t fv_media_mem_audio_append(int32_t handle, moonbit_bytes_t data_bytes) {
  AvAudioStream *s = get_astream(handle);
  if (!s || !s->is_mem) return 0;
  size_t len = (size_t)Moonbit_array_length(data_bytes);
  if (len == 0) return 0;
  size_t old_len = s->mem.len;
  if (!membuf_append_mse(&s->mem, (const uint8_t *)data_bytes, len)) return 0;
  if (s->opened && (s->eof || s->last_pts_ms < 0 || s->mem.pos >= old_len)) {
    s->eof = 0;
    s->need_reopen = 1;
  }
  return 1;
}

void fv_media_mem_audio_end(int32_t handle) {
  AvAudioStream *s = get_astream(handle);
  if (!s || !s->is_mem) return;
  s->mem_ended = 1;
}

static int astream_append_pcm(AvAudioStream *s, const uint8_t *data, size_t n) {
  if (n == 0) return 1;
  if (s->pcm_len + n > s->pcm_cap) {
    size_t ncap = s->pcm_cap ? s->pcm_cap * 2 : 32768;
    while (ncap < s->pcm_len + n) ncap *= 2;
    uint8_t *nb = (uint8_t *)realloc(s->pcm_buf, ncap);
    if (!nb) return 0;
    s->pcm_buf = nb;
    s->pcm_cap = ncap;
  }
  memcpy(s->pcm_buf + s->pcm_len, data, n);
  s->pcm_len += n;
  return 1;
}

moonbit_bytes_t fv_av_audio_read(int32_t handle, int32_t max_bytes) {
  AvAudioStream *s = get_astream(handle);
  if (!s || max_bytes <= 0) return moonbit_make_bytes(0, 0);
  if (max_bytes > 65536) max_bytes = 65536;

  if (s->is_mem && (!s->opened || s->need_reopen)) {
    if (s->mem.len == 0) return moonbit_make_bytes(0, 0);
    int32_t resume = (s->need_reopen && s->last_pts_ms >= 0) ? (int32_t)(s->last_pts_ms + 1) : 0;
    if (av_astream_open_internal(s, resume) < 0) return moonbit_make_bytes(0, 0);
  }

  const FvAvAbi *a = &g_av.abi;
  int guard = 256;
  while (s->pcm_len < (size_t)max_bytes && s->opened && !s->eof && guard-- > 0) {
    int ret = g_av.avcodec_receive_frame(s->dec_ctx, s->frame);
    if (ret == 0) {
      int nb = rd_i32(s->frame, a->fr_nb_samples);
      int64_t pts = rd_i64(s->frame, a->fr_pts);
      int64_t pms = pts_to_ms(pts, s->time_base);
      if (pms < 0) pms = (s->last_pts_ms >= 0) ? s->last_pts_ms : 0;
      if (s->min_pts_ms >= 0 && pms < s->min_pts_ms) {
        g_av.av_frame_unref(s->frame);
        continue;
      }
      s->min_pts_ms = -1;
      if (nb > 0) {
        int in_sr = rd_i32(s->dec_ctx, a->cc_sample_rate);
        if (in_sr <= 0) in_sr = 48000;
        int64_t delay = g_av.swr_get_delay(s->swr, in_sr);
        int out_cap = (int)(((delay + nb) * 48000) / in_sr) + 64;
        uint8_t *tmp = (uint8_t *)malloc((size_t)out_cap * 4);
        if (tmp) {
          uint8_t *out_planes[1] = { tmp };
          const uint8_t **in_planes = (const uint8_t **)((uint8_t *)s->frame + a->fr_data);
          int got = g_av.swr_convert(s->swr, out_planes, out_cap, in_planes, nb);
          if (got > 0) {
            astream_append_pcm(s, tmp, (size_t)got * 4);
            s->last_pts_ms = pms + (int64_t)got * 1000 / 48000;
          }
          free(tmp);
        }
      }
      g_av.av_frame_unref(s->frame);
      continue;
    }
    if (ret == FV_AVERROR_EOF) {
      // Flush resampler tail
      uint8_t tail[4096];
      uint8_t *out_planes[1] = { tail };
      int got = g_av.swr_convert(s->swr, out_planes, 1024, NULL, 0);
      if (got > 0) astream_append_pcm(s, tail, (size_t)got * 4);
      s->eof = 1;
      break;
    }
    if (ret != FV_AVERROR_EAGAIN && ret < 0) {
      s->eof = 1;
      break;
    }
    int got_pkt = 0;
    while (g_av.av_read_frame(s->ic, s->pkt) >= 0) {
      if (rd_i32(s->pkt, a->pkt_stream_index) == s->audio_idx) {
        g_av.avcodec_send_packet(s->dec_ctx, s->pkt);
        g_av.av_packet_unref(s->pkt);
        got_pkt = 1;
        break;
      }
      g_av.av_packet_unref(s->pkt);
    }
    if (!got_pkt) {
      g_av.avcodec_send_packet(s->dec_ctx, NULL);
    }
  }

  size_t take = s->pcm_len < (size_t)max_bytes ? s->pcm_len : (size_t)max_bytes;
  // Align to 4-byte stereo s16 frame boundary
  take &= ~(size_t)3;
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)take, 0);
  if (take > 0) {
    memcpy(res, s->pcm_buf, take);
    if (s->out_handle > 0) {
      fv_audio_out_write(s->out_handle, (const uint8_t *)res, (int32_t)take);
    }
    size_t rem = s->pcm_len - take;
    if (rem > 0) memmove(s->pcm_buf, s->pcm_buf + take, rem);
    s->pcm_len = rem;
  }
  return res;
}

int32_t fv_media_audio_last_pts_ms(int32_t handle) {
  AvAudioStream *s = get_astream(handle);
  if (!s || s->last_pts_ms < 0) return -1;
  return (int32_t)s->last_pts_ms;
}

int32_t fv_media_audio_seek(int32_t handle, int32_t target_ms) {
  AvAudioStream *s = get_astream(handle);
  if (!s) return 0;
  if (target_ms < 0) target_ms = 0;
  s->pcm_len = 0;
  if (!s->opened || s->need_reopen || s->eof) {
    return av_astream_open_internal(s, target_ms) == 0 ? 1 : 0;
  }
  int64_t ts = g_av.av_rescale_q((int64_t)target_ms, (FvAVRational){1, 1000}, s->time_base);
  if (g_av.av_seek_frame(s->ic, s->audio_idx, ts, FV_AVSEEK_FLAG_BACKWARD) < 0) {
    return av_astream_open_internal(s, target_ms) == 0 ? 1 : 0;
  }
  g_av.avcodec_flush_buffers(s->dec_ctx);
  s->eof = 0;
  s->min_pts_ms = target_ms;
  return 1;
}

int32_t fv_av_audio_eof(int32_t handle) {
  AvAudioStream *s = get_astream(handle);
  if (!s) return 1;
  if (s->pcm_len >= 4) return 0;
  if (s->is_mem && !s->mem_ended) return 0;
  return s->eof ? 1 : 0;
}

void fv_av_audio_close(int32_t handle) {
  av_astream_close_slot(handle - FV_AV_HANDLE_BASE);
}

int32_t fv_av_audio_count(void) {
  int n = 0;
  for (int i = 0; i < FV_AV_MAX_STREAMS; i++) {
    if (g_av_astreams[i].in_use) n++;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Real AudioContext.decodeAudioData backend -> Float32 planar PCM
// Format: [u32_le 0x46564144][u32_le sample_rate][u32_le channels][u32_le frames][ch0 f32...][ch1 f32...]
// ---------------------------------------------------------------------------

moonbit_bytes_t fv_media_decode_audio(moonbit_bytes_t data_bytes) {
  if (!fv_media_inproc_available()) return moonbit_make_bytes(0, 0);
  size_t len = (size_t)Moonbit_array_length(data_bytes);
  if (len == 0) return moonbit_make_bytes(0, 0);

  const FvAvAbi *a = &g_av.abi;
  FvMemBuf m = { (uint8_t *)data_bytes, len, len, 0, 0 };
  void *ic = NULL, *pb = NULL;
  if (open_input_mem(&m, &ic, &pb) < 0) return moonbit_make_bytes(0, 0);

  const void *dec = NULL;
  int aidx = g_av.av_find_best_stream(ic, FV_AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if (aidx < 0 || !dec) {
    close_input_mem(&ic, &pb);
    return moonbit_make_bytes(0, 0);
  }

  void **streams = (void **)rd_ptr(ic, a->fc_streams);
  void *st = streams[aidx];
  void *cp = rd_ptr(st, a->st_codecpar);

  void *cc = g_av.avcodec_alloc_context3(dec);
  if (!cc || g_av.avcodec_parameters_to_context(cc, cp) < 0 || g_av.avcodec_open2(cc, dec, NULL) < 0) {
    if (cc) g_av.avcodec_free_context(&cc);
    close_input_mem(&ic, &pb);
    return moonbit_make_bytes(0, 0);
  }

  int sr = rd_i32(cc, a->cc_sample_rate);
  int in_fmt = rd_i32(cc, a->cc_sample_fmt);
  FvAVChannelLayout in_ch;
  memcpy(&in_ch, (const uint8_t *)cc + a->cc_ch_layout, sizeof(in_ch));
  if (sr <= 0) sr = 48000;
  int ch = in_ch.nb_channels;
  FvAVChannelLayout fallback_in_ch;
  memset(&fallback_in_ch, 0, sizeof(fallback_in_ch));
  if (ch <= 0) {
    ch = 2;
    g_av.av_channel_layout_default(&fallback_in_ch, ch);
    in_ch = fallback_in_ch;
  }
  if (ch > 8) ch = 8;

  FvAVChannelLayout out_ch;
  memset(&out_ch, 0, sizeof(out_ch));
  g_av.av_channel_layout_default(&out_ch, ch);

  void *swr = NULL;
  int ret = g_av.swr_alloc_set_opts2(&swr, &out_ch, g_av.sample_fmt_fltp, sr,
                                     &in_ch, in_fmt, sr, 0, NULL);
  g_av.av_channel_layout_uninit(&out_ch);
  if (fallback_in_ch.nb_channels > 0) g_av.av_channel_layout_uninit(&fallback_in_ch);
  if (ret < 0 || !swr || g_av.swr_init(swr) < 0) {
    if (swr) g_av.swr_free(&swr);
    g_av.avcodec_free_context(&cc);
    close_input_mem(&ic, &pb);
    return moonbit_make_bytes(0, 0);
  }

  float *planes[8] = { NULL };
  size_t frames = 0;
  size_t cap = 16384;
  int alloc_ok = 1;
  for (int c = 0; c < ch; c++) {
    planes[c] = (float *)malloc(cap * sizeof(float));
    if (!planes[c]) alloc_ok = 0;
  }

  void *pkt = g_av.av_packet_alloc();
  void *frame = g_av.av_frame_alloc();
  if (!alloc_ok || !pkt || !frame) {
    for (int c = 0; c < ch; c++) free(planes[c]);
    if (pkt) g_av.av_packet_free(&pkt);
    if (frame) g_av.av_frame_free(&frame);
    g_av.swr_free(&swr);
    g_av.avcodec_free_context(&cc);
    close_input_mem(&ic, &pb);
    return moonbit_make_bytes(0, 0);
  }

  int draining = 0;
  while (frames < FV_AV_DECODE_AUDIO_MAX_SAMPLES) {
    if (!draining) {
      int r = g_av.av_read_frame(ic, pkt);
      if (r < 0) {
        g_av.avcodec_send_packet(cc, NULL);
        draining = 1;
      } else {
        if (rd_i32(pkt, a->pkt_stream_index) == aidx) {
          g_av.avcodec_send_packet(cc, pkt);
        }
        g_av.av_packet_unref(pkt);
      }
    }

    int got_any = 0;
    while (g_av.avcodec_receive_frame(cc, frame) == 0) {
      got_any = 1;
      int nb = rd_i32(frame, a->fr_nb_samples);
      if (nb > 0) {
        int out_need = nb + 256;
        if (frames + (size_t)out_need > cap) {
          size_t ncap = cap * 2;
          while (ncap < frames + (size_t)out_need) ncap *= 2;
          if (ncap > FV_AV_DECODE_AUDIO_MAX_SAMPLES) ncap = FV_AV_DECODE_AUDIO_MAX_SAMPLES;
          int ok = 1;
          for (int c = 0; c < ch; c++) {
            float *np = (float *)realloc(planes[c], ncap * sizeof(float));
            if (!np) { ok = 0; break; }
            planes[c] = np;
          }
          if (!ok) { g_av.av_frame_unref(frame); goto finish; }
          cap = ncap;
        }
        int avail = (int)(cap - frames);
        if (avail > 0) {
          uint8_t *out_ptrs[8] = { NULL };
          for (int c = 0; c < ch; c++) out_ptrs[c] = (uint8_t *)(planes[c] + frames);
          const uint8_t **in_ptrs = (const uint8_t **)((uint8_t *)frame + a->fr_data);
          int got = g_av.swr_convert(swr, out_ptrs, avail, in_ptrs, nb);
          if (got > 0) frames += (size_t)got;
        }
      }
      g_av.av_frame_unref(frame);
    }
    if (draining && !got_any) break;
  }

  // Flush resampler
  if (frames < cap) {
    uint8_t *out_ptrs[8] = { NULL };
    for (int c = 0; c < ch; c++) out_ptrs[c] = (uint8_t *)(planes[c] + frames);
    int got = g_av.swr_convert(swr, out_ptrs, (int)(cap - frames), NULL, 0);
    if (got > 0) frames += (size_t)got;
  }

finish:
  g_av.av_packet_free(&pkt);
  g_av.av_frame_free(&frame);
  g_av.swr_free(&swr);
  g_av.avcodec_free_context(&cc);
  close_input_mem(&ic, &pb);

  moonbit_bytes_t res;
  size_t payload_bytes = frames * (size_t)ch * sizeof(float);
  if (frames == 0 || payload_bytes + 16 > (size_t)INT32_MAX) {
    res = moonbit_make_bytes(0, 0);
  } else {
    res = moonbit_make_bytes((int32_t)(16 + payload_bytes), 0);
    uint32_t magic = 0x46564144u; // "FVAD"
    uint32_t usr = (uint32_t)sr;
    uint32_t uch = (uint32_t)ch;
    uint32_t ufr = (uint32_t)frames;
    memcpy(res + 0, &magic, 4);
    memcpy(res + 4, &usr, 4);
    memcpy(res + 8, &uch, 4);
    memcpy(res + 12, &ufr, 4);
    size_t ch_bytes = frames * sizeof(float);
    for (int c = 0; c < ch; c++) {
      memcpy(res + 16 + (size_t)c * ch_bytes, planes[c], ch_bytes);
    }
  }
  for (int c = 0; c < ch; c++) free(planes[c]);
  return res;
}

int32_t fv_media_audio_attach_output(int32_t audio_handle, int32_t out_handle) {
  AvAudioStream *s = get_astream(audio_handle);
  if (!s) return 0;
  s->out_handle = out_handle > 0 ? out_handle : 0;
  return 1;
}

int32_t fv_media_play_decoded_audio(moonbit_bytes_t fvad_bytes, int32_t out_handle) {
  if (out_handle <= 0 || !fvad_bytes) return -1;
  size_t len = (size_t)Moonbit_array_length(fvad_bytes);
  if (len < 16) return -1;
  uint32_t magic = 0, sr = 0, ch = 0, frames = 0;
  memcpy(&magic, fvad_bytes + 0, 4);
  memcpy(&sr, fvad_bytes + 4, 4);
  memcpy(&ch, fvad_bytes + 8, 4);
  memcpy(&frames, fvad_bytes + 12, 4);
  (void)sr;
  if (magic != 0x46564144u || ch == 0 || ch > 8 || frames == 0) return -1;
  size_t need = 16 + (size_t)frames * (size_t)ch * sizeof(float);
  if (len < need) return -1;
  const float *ch0 = (const float *)(fvad_bytes + 16);
  const float *ch1 = (ch >= 2) ? (const float *)(fvad_bytes + 16 + (size_t)frames * sizeof(float)) : ch0;
  return fv_audio_out_write_f32_planar(out_handle, ch0, ch1, (int32_t)frames);
}
