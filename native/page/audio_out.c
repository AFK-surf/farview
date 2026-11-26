// Farview native real-time PCM audio output engine.
// macOS: dlopen AudioToolbox.framework (AudioQueue), 0 hard-linked framework dependencies.
// Lock-free SPSC ring buffer between producer writes and real-time audio callback.
#include "audio_out.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../win_compat.h"
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

#define FV_AUDIO_OUT_MAX_SLOTS 16
#define FV_AUDIO_RING_CAP (262144u) /* 256 KiB power-of-2 (~1.36s @ 48kHz stereo s16, ~0.68s f32) */
#define FV_AUDIO_RING_MASK (FV_AUDIO_RING_CAP - 1u)
#define FV_AUDIO_AQ_NUM_BUFS 3

/* --------------------------------------------------------------------------
 * Dynamic AudioToolbox.framework ABI (macOS only, zero hard-link deps)
 * -------------------------------------------------------------------------- */
#ifdef __APPLE__

typedef int32_t FvOSStatus;
typedef uint32_t FvAudioQueueParameterID;
typedef float FvAudioQueueParameterValue;
typedef struct OpaqueAudioQueue *FvAudioQueueRef;

#define FV_K_AUDIO_FORMAT_LINEAR_PCM      0x6C70636Du /* 'lpcm' */
#define FV_K_LPCM_FLAG_IS_FLOAT           (1u << 0)
#define FV_K_LPCM_FLAG_IS_SIGNED_INTEGER  (1u << 2)
#define FV_K_LPCM_FLAG_IS_PACKED          (1u << 3)
#define FV_K_AQ_PARAM_VOLUME              1u

typedef struct {
  double   mSampleRate;
  uint32_t mFormatID;
  uint32_t mFormatFlags;
  uint32_t mBytesPerPacket;
  uint32_t mFramesPerPacket;
  uint32_t mBytesPerFrame;
  uint32_t mChannelsPerFrame;
  uint32_t mBitsPerChannel;
  uint32_t mReserved;
} FvAudioStreamBasicDescription;

/* Exact macOS 64-bit AudioQueueBuffer layout (verified against AudioQueue.h) */
typedef struct FvAudioQueueBuffer {
  uint32_t mAudioDataBytesCapacity;
  void    *mAudioData;
  uint32_t mAudioDataByteSize;
  void    *mUserData;
  uint32_t mPacketDescriptionCapacity;
  void    *mPacketDescriptions;
  uint32_t mPacketDescriptionCount;
} FvAudioQueueBuffer;

typedef void (*FvAudioQueueOutputCallback)(void *inUserData, FvAudioQueueRef inAQ,
                                           FvAudioQueueBuffer *inBuffer);

typedef struct {
  int initialized;
  int available;
  void *handle;
  FvOSStatus (*AudioQueueNewOutput)(const FvAudioStreamBasicDescription *inFormat,
                                    FvAudioQueueOutputCallback inCallbackProc,
                                    void *inUserData,
                                    void *inCallbackRunLoop,
                                    void *inCallbackRunLoopMode,
                                    uint32_t inFlags,
                                    FvAudioQueueRef *outAQ);
  FvOSStatus (*AudioQueueAllocateBuffer)(FvAudioQueueRef inAQ,
                                         uint32_t inBufferByteSize,
                                         FvAudioQueueBuffer **outBuffer);
  FvOSStatus (*AudioQueueEnqueueBuffer)(FvAudioQueueRef inAQ,
                                        FvAudioQueueBuffer *inBuffer,
                                        uint32_t inNumPacketDescs,
                                        const void *inPacketDescs);
  FvOSStatus (*AudioQueueStart)(FvAudioQueueRef inAQ, const void *inStartTime);
  FvOSStatus (*AudioQueuePause)(FvAudioQueueRef inAQ);
  FvOSStatus (*AudioQueueStop)(FvAudioQueueRef inAQ, uint8_t inImmediate);
  FvOSStatus (*AudioQueueFlush)(FvAudioQueueRef inAQ);
  FvOSStatus (*AudioQueueReset)(FvAudioQueueRef inAQ);
  FvOSStatus (*AudioQueueDispose)(FvAudioQueueRef inAQ, uint8_t inImmediate);
  FvOSStatus (*AudioQueueSetParameter)(FvAudioQueueRef inAQ,
                                       FvAudioQueueParameterID inParamID,
                                       FvAudioQueueParameterValue inValue);
} FvAudioToolboxLib;

static FvAudioToolboxLib g_at;
static pthread_once_t g_at_once = PTHREAD_ONCE_INIT;

static void init_audiotoolbox(void) {
  g_at.initialized = 1;
  g_at.available = 0;
  g_at.handle = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",
                       RTLD_LAZY | RTLD_LOCAL);
  if (!g_at.handle) return;

#define LOAD_AT(fn) do { \
  *(void **)(&g_at.fn) = dlsym(g_at.handle, #fn); \
  if (!g_at.fn) return; \
} while (0)

  LOAD_AT(AudioQueueNewOutput);
  LOAD_AT(AudioQueueAllocateBuffer);
  LOAD_AT(AudioQueueEnqueueBuffer);
  LOAD_AT(AudioQueueStart);
  LOAD_AT(AudioQueuePause);
  LOAD_AT(AudioQueueStop);
  LOAD_AT(AudioQueueFlush);
  LOAD_AT(AudioQueueReset);
  LOAD_AT(AudioQueueDispose);
  LOAD_AT(AudioQueueSetParameter);

#undef LOAD_AT
  g_at.available = 1;
}

static int audiotoolbox_ready(void) {
  pthread_once(&g_at_once, init_audiotoolbox);
  return g_at.available;
}

static void fill_asbd(FvAudioStreamBasicDescription *asbd, int32_t sr, int32_t ch, int32_t fmt) {
  memset(asbd, 0, sizeof(*asbd));
  uint32_t bps = (fmt == FV_AUDIO_FMT_F32) ? 4u : 2u;
  uint32_t bpf = bps * (uint32_t)ch;
  asbd->mSampleRate = (double)sr;
  asbd->mFormatID = FV_K_AUDIO_FORMAT_LINEAR_PCM;
  asbd->mFormatFlags = (fmt == FV_AUDIO_FMT_F32)
      ? (FV_K_LPCM_FLAG_IS_FLOAT | FV_K_LPCM_FLAG_IS_PACKED)
      : (FV_K_LPCM_FLAG_IS_SIGNED_INTEGER | FV_K_LPCM_FLAG_IS_PACKED);
  asbd->mBytesPerPacket = bpf;
  asbd->mFramesPerPacket = 1;
  asbd->mBytesPerFrame = bpf;
  asbd->mChannelsPerFrame = (uint32_t)ch;
  asbd->mBitsPerChannel = bps * 8u;
  asbd->mReserved = 0;
}

#endif /* __APPLE__ */

#ifdef _WIN32

#define FV_WINMM_WAVE_MAPPER       ((UINT)-1)
#define FV_WINMM_WAVE_FORMAT_PCM   1
#define FV_WINMM_WAVE_FORMAT_FLOAT 3
#define FV_WINMM_CALLBACK_EVENT    0x00050000u
#define FV_WINMM_WHDR_DONE         0x00000001u
#define FV_WINMM_WHDR_PREPARED     0x00000002u

typedef void *FvHWAVEOUT;
typedef UINT FvMMRESULT;

#pragma pack(push, 1)
typedef struct {
  WORD  wFormatTag;
  WORD  nChannels;
  DWORD nSamplesPerSec;
  DWORD nAvgBytesPerSec;
  WORD  nBlockAlign;
  WORD  wBitsPerSample;
  WORD  cbSize;
} FvWAVEFORMATEX;
#pragma pack(pop)

typedef struct fv_wavehdr_tag {
  LPSTR lpData;
  DWORD dwBufferLength;
  DWORD dwBytesRecorded;
  DWORD_PTR dwUser;
  DWORD dwFlags;
  DWORD dwLoops;
  struct fv_wavehdr_tag *lpNext;
  DWORD_PTR reserved;
} FvWAVEHDR;

typedef struct {
  int initialized;
  int available;
  void *handle;
  UINT (WINAPI *waveOutGetNumDevs)(void);
  FvMMRESULT (WINAPI *waveOutOpen)(FvHWAVEOUT *phwo, UINT uDeviceID, const FvWAVEFORMATEX *pwfx,
                                   DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);
  FvMMRESULT (WINAPI *waveOutPrepareHeader)(FvHWAVEOUT hwo, FvWAVEHDR *pwh, UINT cbwh);
  FvMMRESULT (WINAPI *waveOutUnprepareHeader)(FvHWAVEOUT hwo, FvWAVEHDR *pwh, UINT cbwh);
  FvMMRESULT (WINAPI *waveOutWrite)(FvHWAVEOUT hwo, FvWAVEHDR *pwh, UINT cbwh);
  FvMMRESULT (WINAPI *waveOutPause)(FvHWAVEOUT hwo);
  FvMMRESULT (WINAPI *waveOutRestart)(FvHWAVEOUT hwo);
  FvMMRESULT (WINAPI *waveOutReset)(FvHWAVEOUT hwo);
  FvMMRESULT (WINAPI *waveOutClose)(FvHWAVEOUT hwo);
} FvWinMMLib;

static FvWinMMLib g_winmm;
static pthread_once_t g_winmm_once = PTHREAD_ONCE_INIT;

static void init_winmm(void) {
  g_winmm.initialized = 1;
  g_winmm.available = 0;
  g_winmm.handle = dlopen("winmm.dll", RTLD_LAZY | RTLD_LOCAL);
  if (!g_winmm.handle) return;

#define LOAD_WM(fn) do { \
  *(void **)(&g_winmm.fn) = dlsym(g_winmm.handle, #fn); \
  if (!g_winmm.fn) return; \
} while (0)

  LOAD_WM(waveOutGetNumDevs);
  LOAD_WM(waveOutOpen);
  LOAD_WM(waveOutPrepareHeader);
  LOAD_WM(waveOutUnprepareHeader);
  LOAD_WM(waveOutWrite);
  LOAD_WM(waveOutPause);
  LOAD_WM(waveOutRestart);
  LOAD_WM(waveOutReset);
  LOAD_WM(waveOutClose);

#undef LOAD_WM
  g_winmm.available = 1;
}

static int winmm_ready(void) {
  pthread_once(&g_winmm_once, init_winmm);
  return g_winmm.available;
}

static void fill_wfx(FvWAVEFORMATEX *wfx, int32_t sr, int32_t ch, int32_t fmt) {
  memset(wfx, 0, sizeof(*wfx));
  WORD bps = (fmt == FV_AUDIO_FMT_F32) ? 4 : 2;
  WORD block_align = (WORD)(bps * (WORD)ch);
  wfx->wFormatTag = (fmt == FV_AUDIO_FMT_F32) ? FV_WINMM_WAVE_FORMAT_FLOAT : FV_WINMM_WAVE_FORMAT_PCM;
  wfx->nChannels = (WORD)ch;
  wfx->nSamplesPerSec = (DWORD)sr;
  wfx->nBlockAlign = block_align;
  wfx->nAvgBytesPerSec = (DWORD)sr * (DWORD)block_align;
  wfx->wBitsPerSample = (WORD)(bps * 8);
  wfx->cbSize = 0;
}

#endif /* _WIN32 */

/* --------------------------------------------------------------------------
 * Audio Output Slot & Lock-Free SPSC Ring Buffer
 * -------------------------------------------------------------------------- */

typedef struct {
  int used;
  int32_t backend;
  int32_t sample_rate;
  int32_t channels;
  int32_t format;
  int32_t bytes_per_frame;
  int32_t buf_bytes; /* 20ms chunk size in bytes */

  /* Lock-free ring buffer (producer writes head, audio consumer is sole writer of tail) */
  uint8_t *ring;
  _Atomic uint32_t head;
  _Atomic uint32_t tail;
  _Atomic uint32_t flush_target;
  _Atomic int32_t  flush_pending;
  pthread_mutex_t write_mu; /* serializes multiple producer threads only; never locked on RT cb */

  /* Atomic playback controls */
  _Atomic int32_t running;
  _Atomic int32_t stopping;
  _Atomic int32_t muted;
  _Atomic float   volume;

  /* Atomic stats */
  _Atomic int64_t frames_written;
  _Atomic int64_t frames_played;
  _Atomic int64_t callbacks;
  _Atomic int64_t underruns;
  _Atomic int64_t overruns;
  int32_t last_osstatus;

#ifdef __APPLE__
  FvAudioQueueRef queue;
  FvAudioQueueBuffer *aq_bufs[FV_AUDIO_AQ_NUM_BUFS];
#endif

#ifdef _WIN32
  FvHWAVEOUT hwo;
  HANDLE winmm_event;
  FvWAVEHDR winmm_hdrs[FV_AUDIO_AQ_NUM_BUFS];
  uint8_t *winmm_bufs[FV_AUDIO_AQ_NUM_BUFS];
  int winmm_thread_started;
  pthread_t winmm_thread;
#endif

  /* Null-sink background consumer thread */
  int null_thread_started;
  pthread_t null_thread;
} FvAudioOutSlot;

static pthread_mutex_t g_ao_mu = PTHREAD_MUTEX_INITIALIZER;
static FvAudioOutSlot g_ao_slots[FV_AUDIO_OUT_MAX_SLOTS];

static inline uint32_t ring_available(const FvAudioOutSlot *s) {
  if (atomic_load_explicit(&s->flush_pending, memory_order_acquire) != 0) {
    return 0;
  }
  uint32_t h = atomic_load_explicit(&s->head, memory_order_acquire);
  uint32_t t = atomic_load_explicit(&s->tail, memory_order_relaxed);
  return h - t;
}

static inline uint32_t ring_free_space(const FvAudioOutSlot *s) {
  uint32_t h = atomic_load_explicit(&s->head, memory_order_relaxed);
  uint32_t t = atomic_load_explicit(&s->tail, memory_order_acquire);
  return FV_AUDIO_RING_CAP - (h - t);
}

/* Lock-free consumer read executed on AudioQueue RT thread or null-sink thread. */
static uint32_t ring_consume(FvAudioOutSlot *s, uint8_t *dst, uint32_t want_bytes) {
  uint32_t avail = ring_available(s);
  uint32_t n = avail < want_bytes ? avail : want_bytes;
  if (s->bytes_per_frame > 0) {
    n -= (n % (uint32_t)s->bytes_per_frame);
  }
  if (n > 0) {
    uint32_t t = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint32_t idx = t & FV_AUDIO_RING_MASK;
    uint32_t first = FV_AUDIO_RING_CAP - idx;
    if (first >= n) {
      memcpy(dst, s->ring + idx, n);
    } else {
      memcpy(dst, s->ring + idx, first);
      memcpy(dst + first, s->ring, n - first);
    }
    atomic_store_explicit(&s->tail, t + n, memory_order_release);
  }
  return n;
}

static void apply_gain_and_mute(uint8_t *buf, uint32_t nbytes, int32_t fmt, int32_t muted, float vol) {
  if (nbytes == 0) return;
  if (muted || vol <= 0.0001f) {
    memset(buf, 0, nbytes);
    return;
  }
  if (vol >= 0.9999f) return;
  if (fmt == FV_AUDIO_FMT_F32) {
    float *fp = (float *)buf;
    uint32_t cnt = nbytes / 4u;
    for (uint32_t i = 0; i < cnt; i++) {
      fp[i] *= vol;
    }
  } else {
    int16_t *sp = (int16_t *)buf;
    uint32_t cnt = nbytes / 2u;
    for (uint32_t i = 0; i < cnt; i++) {
      float v = (float)sp[i] * vol;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      sp[i] = (int16_t)v;
    }
  }
}

/* Fill one output buffer from the ring (used by both CoreAudio callback and null sink). */
static void render_audio_buffer(FvAudioOutSlot *s, uint8_t *dst, uint32_t cap_bytes) {
  /* Consumer is the sole writer of s->tail: apply any pending flush request safely. */
  if (atomic_exchange_explicit(&s->flush_pending, 0, memory_order_acq_rel)) {
    uint32_t ft = atomic_load_explicit(&s->flush_target, memory_order_acquire);
    atomic_store_explicit(&s->tail, ft, memory_order_release);
  }
  atomic_fetch_add_explicit(&s->callbacks, 1, memory_order_relaxed);
  int32_t running = atomic_load_explicit(&s->running, memory_order_acquire);
  if (!running) {
    memset(dst, 0, cap_bytes);
    return;
  }
  uint32_t got = ring_consume(s, dst, cap_bytes);
  if (got < cap_bytes) {
    memset(dst + got, 0, cap_bytes - got);
    if (got == 0) {
      atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
    }
  }
  if (got > 0) {
    int32_t muted = atomic_load_explicit(&s->muted, memory_order_relaxed);
    float vol = atomic_load_explicit(&s->volume, memory_order_relaxed);
    apply_gain_and_mute(dst, got, s->format, muted, vol);
    if (s->bytes_per_frame > 0) {
      atomic_fetch_add_explicit(&s->frames_played, (int64_t)(got / (uint32_t)s->bytes_per_frame),
                                memory_order_relaxed);
    }
  }
}

#ifdef __APPLE__
static void aq_output_callback(void *inUserData, FvAudioQueueRef inAQ,
                               FvAudioQueueBuffer *inBuffer) {
  FvAudioOutSlot *s = (FvAudioOutSlot *)inUserData;
  if (!s || !inBuffer || !inBuffer->mAudioData) return;
  if (atomic_load_explicit(&s->stopping, memory_order_acquire)) return;

  uint32_t cap = inBuffer->mAudioDataBytesCapacity;
  render_audio_buffer(s, (uint8_t *)inBuffer->mAudioData, cap);
  inBuffer->mAudioDataByteSize = cap;

  if (!atomic_load_explicit(&s->stopping, memory_order_acquire)) {
    g_at.AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
  }
}
#endif

#ifdef _WIN32
static void *winmm_feeder_thread_main(void *arg) {
  FvAudioOutSlot *s = (FvAudioOutSlot *)arg;
  while (!atomic_load_explicit(&s->stopping, memory_order_acquire)) {
    int submitted = 0;
    for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) {
      if (atomic_load_explicit(&s->stopping, memory_order_acquire)) break;
      if ((s->winmm_hdrs[b].dwFlags & FV_WINMM_WHDR_DONE) != 0) {
        render_audio_buffer(s, s->winmm_bufs[b], (uint32_t)s->buf_bytes);
        s->winmm_hdrs[b].dwBufferLength = (DWORD)s->buf_bytes;
        s->winmm_hdrs[b].dwFlags &= ~FV_WINMM_WHDR_DONE;
        FvMMRESULT mr = g_winmm.waveOutWrite(s->hwo, &s->winmm_hdrs[b], sizeof(FvWAVEHDR));
        s->last_osstatus = (int32_t)mr;
        submitted = 1;
      }
    }
    if (!submitted && !atomic_load_explicit(&s->stopping, memory_order_acquire)) {
      WaitForSingleObject(s->winmm_event, 20);
    }
  }
  return NULL;
}
#endif

static void *null_sink_thread_main(void *arg) {
  FvAudioOutSlot *s = (FvAudioOutSlot *)arg;
  uint32_t chunk = (uint32_t)s->buf_bytes;
  if (chunk == 0 || chunk > 65536u) chunk = 3840u;
  uint8_t *scratch = (uint8_t *)malloc(chunk);
  if (!scratch) return NULL;

  while (!atomic_load_explicit(&s->stopping, memory_order_acquire)) {
    if (atomic_load_explicit(&s->running, memory_order_acquire) ||
        atomic_load_explicit(&s->flush_pending, memory_order_acquire)) {
      render_audio_buffer(s, scratch, chunk);
    }
    /* 10ms tick so tests complete promptly while exercising real consumer thread */
    usleep(10000);
  }
  free(scratch);
  return NULL;
}

static int env_audio_null_enabled(void) {
  const char *v = getenv("FV_AUDIO_NULL");
  return (v && strcmp(v, "1") == 0) ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int32_t fv_audio_out_probe_hardware(void) {
#ifdef __APPLE__
  if (!audiotoolbox_ready()) return 0;
  FvAudioStreamBasicDescription asbd;
  fill_asbd(&asbd, 48000, 2, FV_AUDIO_FMT_F32);
  FvAudioQueueRef q = NULL;
  FvOSStatus st = g_at.AudioQueueNewOutput(&asbd, aq_output_callback, NULL, NULL, NULL, 0, &q);
  if (st != 0 || !q) return 0;
  FvAudioQueueBuffer *buf = NULL;
  st = g_at.AudioQueueAllocateBuffer(q, 960 * 8, &buf);
  g_at.AudioQueueDispose(q, 1);
  return (st == 0 && buf != NULL) ? 1 : 0;
#elif defined(_WIN32)
  if (!winmm_ready()) return 0;
  if (g_winmm.waveOutGetNumDevs() == 0) return 0;
  FvWAVEFORMATEX wfx;
  fill_wfx(&wfx, 48000, 2, FV_AUDIO_FMT_S16);
  FvHWAVEOUT hwo = NULL;
  FvMMRESULT mr = g_winmm.waveOutOpen(&hwo, FV_WINMM_WAVE_MAPPER, &wfx, 0, 0, 0);
  if (mr != 0 || !hwo) return 0;
  g_winmm.waveOutClose(hwo);
  return 1;
#else
  return 0;
#endif
}

int32_t fv_audio_out_open(int32_t sample_rate, int32_t channels, int32_t fmt, int32_t flags) {
  if (sample_rate < 8000 || sample_rate > 192000) sample_rate = 48000;
  if (channels < 1 || channels > 2) channels = 2;
  if (fmt != FV_AUDIO_FMT_S16 && fmt != FV_AUDIO_FMT_F32) fmt = FV_AUDIO_FMT_S16;

  pthread_mutex_lock(&g_ao_mu);
  int slot_idx = -1;
  for (int i = 0; i < FV_AUDIO_OUT_MAX_SLOTS; i++) {
    if (!g_ao_slots[i].used) {
      slot_idx = i;
      break;
    }
  }
  if (slot_idx < 0) {
    pthread_mutex_unlock(&g_ao_mu);
    return -1;
  }

  FvAudioOutSlot *s = &g_ao_slots[slot_idx];
  memset(s, 0, sizeof(*s));
  s->ring = (uint8_t *)calloc(1, FV_AUDIO_RING_CAP);
  if (!s->ring) {
    pthread_mutex_unlock(&g_ao_mu);
    return -1;
  }
  pthread_mutex_init(&s->write_mu, NULL);
  s->used = 1;
  s->sample_rate = sample_rate;
  s->channels = channels;
  s->format = fmt;
  s->bytes_per_frame = channels * ((fmt == FV_AUDIO_FMT_F32) ? 4 : 2);
  /* 20ms buffer chunk aligned to frame size */
  int32_t frames_20ms = sample_rate / 50;
  if (frames_20ms < 128) frames_20ms = 128;
  s->buf_bytes = frames_20ms * s->bytes_per_frame;

  atomic_init(&s->head, 0);
  atomic_init(&s->tail, 0);
  atomic_init(&s->flush_target, 0);
  atomic_init(&s->flush_pending, 0);
  atomic_init(&s->running, 1);
  atomic_init(&s->stopping, 0);
  atomic_init(&s->muted, 0);
  atomic_init(&s->volume, 1.0f);
  atomic_init(&s->frames_written, 0);
  atomic_init(&s->frames_played, 0);
  atomic_init(&s->callbacks, 0);
  atomic_init(&s->underruns, 0);
  atomic_init(&s->overruns, 0);
  s->last_osstatus = 0;

  int force_null = ((flags & FV_AUDIO_OUT_FLAG_NULL) != 0) || env_audio_null_enabled();
  int hw_ok = 0;

#ifdef __APPLE__
  if (!force_null && audiotoolbox_ready()) {
    FvAudioStreamBasicDescription asbd;
    fill_asbd(&asbd, sample_rate, channels, fmt);
    FvOSStatus st = g_at.AudioQueueNewOutput(&asbd, aq_output_callback, s, NULL, NULL, 0, &s->queue);
    s->last_osstatus = st;
    if (st == 0 && s->queue) {
      int alloc_ok = 1;
      for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) {
        st = g_at.AudioQueueAllocateBuffer(s->queue, (uint32_t)s->buf_bytes, &s->aq_bufs[b]);
        if (st != 0 || !s->aq_bufs[b]) {
          s->last_osstatus = st;
          alloc_ok = 0;
          break;
        }
        memset(s->aq_bufs[b]->mAudioData, 0, (size_t)s->buf_bytes);
        s->aq_bufs[b]->mAudioDataByteSize = (uint32_t)s->buf_bytes;
        g_at.AudioQueueEnqueueBuffer(s->queue, s->aq_bufs[b], 0, NULL);
      }
      if (alloc_ok) {
        st = g_at.AudioQueueStart(s->queue, NULL);
        s->last_osstatus = st;
        if (st == 0) {
          s->backend = FV_AUDIO_BACKEND_COREAUDIO;
          hw_ok = 1;
        }
      }
      if (!hw_ok) {
        g_at.AudioQueueDispose(s->queue, 1);
        s->queue = NULL;
        for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) s->aq_bufs[b] = NULL;
      }
    }
  }
#elif defined(_WIN32)
  if (!force_null && winmm_ready() && g_winmm.waveOutGetNumDevs() > 0) {
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (ev) {
      FvWAVEFORMATEX wfx;
      fill_wfx(&wfx, sample_rate, channels, fmt);
      FvHWAVEOUT hwo = NULL;
      FvMMRESULT mr = g_winmm.waveOutOpen(
          &hwo, FV_WINMM_WAVE_MAPPER, &wfx, (DWORD_PTR)ev, 0, FV_WINMM_CALLBACK_EVENT);
      s->last_osstatus = (int32_t)mr;
      if (mr == 0 && hwo) {
        s->hwo = hwo;
        s->winmm_event = ev;
        int prep_ok = 1;
        for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) {
          s->winmm_bufs[b] = (uint8_t *)calloc(1, (size_t)s->buf_bytes);
          if (!s->winmm_bufs[b]) { prep_ok = 0; break; }
          memset(&s->winmm_hdrs[b], 0, sizeof(FvWAVEHDR));
          s->winmm_hdrs[b].lpData = (LPSTR)s->winmm_bufs[b];
          s->winmm_hdrs[b].dwBufferLength = (DWORD)s->buf_bytes;
          mr = g_winmm.waveOutPrepareHeader(s->hwo, &s->winmm_hdrs[b], sizeof(FvWAVEHDR));
          if (mr != 0) {
            s->last_osstatus = (int32_t)mr;
            prep_ok = 0;
            break;
          }
          mr = g_winmm.waveOutWrite(s->hwo, &s->winmm_hdrs[b], sizeof(FvWAVEHDR));
          if (mr != 0) {
            s->last_osstatus = (int32_t)mr;
            prep_ok = 0;
            break;
          }
        }
        if (prep_ok && pthread_create(&s->winmm_thread, NULL, winmm_feeder_thread_main, s) == 0) {
          s->winmm_thread_started = 1;
          s->backend = FV_AUDIO_BACKEND_WINMM;
          hw_ok = 1;
        }
        if (!hw_ok) {
          g_winmm.waveOutReset(s->hwo);
          for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) {
            if (s->winmm_hdrs[b].dwFlags & FV_WINMM_WHDR_PREPARED) {
              g_winmm.waveOutUnprepareHeader(s->hwo, &s->winmm_hdrs[b], sizeof(FvWAVEHDR));
            }
            free(s->winmm_bufs[b]);
            s->winmm_bufs[b] = NULL;
          }
          g_winmm.waveOutClose(s->hwo);
          s->hwo = NULL;
          CloseHandle(ev);
          s->winmm_event = NULL;
        }
      } else {
        CloseHandle(ev);
      }
    }
  }
#else
  (void)force_null;
#endif

  if (!hw_ok) {
    s->backend = FV_AUDIO_BACKEND_NULL;
    if (pthread_create(&s->null_thread, NULL, null_sink_thread_main, s) == 0) {
      s->null_thread_started = 1;
    }
  }

  int32_t handle = slot_idx + 1;
  pthread_mutex_unlock(&g_ao_mu);
  return handle;
}

static FvAudioOutSlot *get_ao_slot(int32_t handle) {
  if (handle < 1 || handle > FV_AUDIO_OUT_MAX_SLOTS) return NULL;
  FvAudioOutSlot *s = &g_ao_slots[handle - 1];
  return s->used ? s : NULL;
}

int32_t fv_audio_out_write(int32_t handle, const uint8_t *data, int32_t len) {
  if (!data || len <= 0) return 0;
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return -1;

  pthread_mutex_lock(&s->write_mu);
  uint32_t bpf = (uint32_t)s->bytes_per_frame;
  uint32_t want = (uint32_t)len;
  if (bpf > 0) want -= (want % bpf);
  uint32_t avail = ring_free_space(s);
  if (bpf > 0) avail -= (avail % bpf);

  uint32_t n = want < avail ? want : avail;
  if (n < want) {
    atomic_fetch_add_explicit(&s->overruns, 1, memory_order_relaxed);
  }
  if (n > 0) {
    uint32_t h = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t idx = h & FV_AUDIO_RING_MASK;
    uint32_t first = FV_AUDIO_RING_CAP - idx;
    if (first >= n) {
      memcpy(s->ring + idx, data, n);
    } else {
      memcpy(s->ring + idx, data, first);
      memcpy(s->ring, data + first, n - first);
    }
    atomic_store_explicit(&s->head, h + n, memory_order_release);
    if (bpf > 0) {
      atomic_fetch_add_explicit(&s->frames_written, (int64_t)(n / bpf), memory_order_relaxed);
    }
  }
  pthread_mutex_unlock(&s->write_mu);
  return (int32_t)n;
}

int32_t fv_audio_out_write_f32_planar(int32_t handle, const float *ch0, const float *ch1,
                                      int32_t frames) {
  if (!ch0 || frames <= 0) return 0;
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return -1;

  const float *r = ch1 ? ch1 : ch0;
  int32_t ch = s->channels;
  int32_t bpf = s->bytes_per_frame;
  int32_t total_written_frames = 0;

  /* Process in 512-frame stack chunks to avoid heap allocation */
  uint8_t tmp[512 * 8];
  int32_t off = 0;
  while (off < frames) {
    int32_t chunk = frames - off;
    if (chunk > 512) chunk = 512;
    if (s->format == FV_AUDIO_FMT_F32) {
      float *dst = (float *)tmp;
      for (int32_t i = 0; i < chunk; i++) {
        if (ch == 1) {
          dst[i] = ch0[off + i];
        } else {
          dst[i * 2] = ch0[off + i];
          dst[i * 2 + 1] = r[off + i];
        }
      }
    } else {
      int16_t *dst = (int16_t *)tmp;
      for (int32_t i = 0; i < chunk; i++) {
        float l = ch0[off + i];
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (ch == 1) {
          dst[i] = (int16_t)(l * 32767.0f);
        } else {
          float rv = r[off + i];
          if (rv > 1.0f) rv = 1.0f;
          if (rv < -1.0f) rv = -1.0f;
          dst[i * 2] = (int16_t)(l * 32767.0f);
          dst[i * 2 + 1] = (int16_t)(rv * 32767.0f);
        }
      }
    }
    int32_t nbytes = fv_audio_out_write(handle, tmp, chunk * bpf);
    if (nbytes <= 0) break;
    int32_t wf = nbytes / bpf;
    total_written_frames += wf;
    if (wf < chunk) break;
    off += chunk;
  }
  return total_written_frames;
}

int32_t fv_audio_out_play(int32_t handle) {
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  atomic_store_explicit(&s->running, 1, memory_order_release);
#ifdef __APPLE__
  if (s->backend == FV_AUDIO_BACKEND_COREAUDIO && s->queue) {
    s->last_osstatus = g_at.AudioQueueStart(s->queue, NULL);
    return s->last_osstatus == 0 ? 1 : 0;
  }
#elif defined(_WIN32)
  if (s->backend == FV_AUDIO_BACKEND_WINMM && s->hwo) {
    s->last_osstatus = (int32_t)g_winmm.waveOutRestart(s->hwo);
    if (s->winmm_event) SetEvent(s->winmm_event);
    return s->last_osstatus == 0 ? 1 : 0;
  }
#endif
  return 1;
}

int32_t fv_audio_out_pause(int32_t handle) {
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  atomic_store_explicit(&s->running, 0, memory_order_release);
#ifdef __APPLE__
  if (s->backend == FV_AUDIO_BACKEND_COREAUDIO && s->queue) {
    s->last_osstatus = g_at.AudioQueuePause(s->queue);
    return s->last_osstatus == 0 ? 1 : 0;
  }
#elif defined(_WIN32)
  if (s->backend == FV_AUDIO_BACKEND_WINMM && s->hwo) {
    s->last_osstatus = (int32_t)g_winmm.waveOutPause(s->hwo);
    return s->last_osstatus == 0 ? 1 : 0;
  }
#endif
  return 1;
}

int32_t fv_audio_out_set_volume(int32_t handle, float volume) {
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  if (volume < 0.0f) volume = 0.0f;
  if (volume > 1.0f) volume = 1.0f;
  atomic_store_explicit(&s->volume, volume, memory_order_relaxed);
#ifdef __APPLE__
  if (s->backend == FV_AUDIO_BACKEND_COREAUDIO && s->queue) {
    int32_t muted = atomic_load_explicit(&s->muted, memory_order_relaxed);
    s->last_osstatus = g_at.AudioQueueSetParameter(s->queue, FV_K_AQ_PARAM_VOLUME,
                                                   muted ? 0.0f : volume);
  }
#endif
  return 1;
}

int32_t fv_audio_out_set_muted(int32_t handle, int32_t muted) {
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  int32_t m = muted ? 1 : 0;
  atomic_store_explicit(&s->muted, m, memory_order_relaxed);
#ifdef __APPLE__
  if (s->backend == FV_AUDIO_BACKEND_COREAUDIO && s->queue) {
    float vol = atomic_load_explicit(&s->volume, memory_order_relaxed);
    s->last_osstatus = g_at.AudioQueueSetParameter(s->queue, FV_K_AQ_PARAM_VOLUME,
                                                   m ? 0.0f : vol);
  }
#endif
  return 1;
}

int32_t fv_audio_out_flush(int32_t handle) {
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  pthread_mutex_lock(&s->write_mu);
  uint32_t h = atomic_load_explicit(&s->head, memory_order_acquire);
  atomic_store_explicit(&s->flush_target, h, memory_order_relaxed);
  atomic_store_explicit(&s->flush_pending, 1, memory_order_release);
  pthread_mutex_unlock(&s->write_mu);
#ifdef __APPLE__
  if (s->backend == FV_AUDIO_BACKEND_COREAUDIO && s->queue) {
    s->last_osstatus = g_at.AudioQueueFlush(s->queue);
  }
#endif
  return 1;
}

void fv_audio_out_close(int32_t handle) {
  if (handle < 1 || handle > FV_AUDIO_OUT_MAX_SLOTS) return;
  pthread_mutex_lock(&g_ao_mu);
  FvAudioOutSlot *s = &g_ao_slots[handle - 1];
  if (!s->used) {
    pthread_mutex_unlock(&g_ao_mu);
    return;
  }
  atomic_store_explicit(&s->stopping, 1, memory_order_release);
  atomic_store_explicit(&s->running, 0, memory_order_release);

#ifdef __APPLE__
  if (s->queue) {
    g_at.AudioQueueStop(s->queue, 1);
    g_at.AudioQueueDispose(s->queue, 1);
    s->queue = NULL;
    for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) s->aq_bufs[b] = NULL;
  }
#endif

#ifdef _WIN32
  if (s->winmm_thread_started) {
    pthread_t wth = s->winmm_thread;
    HANDLE wev = s->winmm_event;
    s->winmm_thread_started = 0;
    if (wev) SetEvent(wev);
    pthread_mutex_unlock(&g_ao_mu);
    pthread_join(wth, NULL);
    pthread_mutex_lock(&g_ao_mu);
  }
  if (s->hwo) {
    g_winmm.waveOutReset(s->hwo);
    for (int b = 0; b < FV_AUDIO_AQ_NUM_BUFS; b++) {
      if (s->winmm_hdrs[b].dwFlags & FV_WINMM_WHDR_PREPARED) {
        g_winmm.waveOutUnprepareHeader(s->hwo, &s->winmm_hdrs[b], sizeof(FvWAVEHDR));
      }
      free(s->winmm_bufs[b]);
      s->winmm_bufs[b] = NULL;
    }
    g_winmm.waveOutClose(s->hwo);
    s->hwo = NULL;
  }
  if (s->winmm_event) {
    CloseHandle(s->winmm_event);
    s->winmm_event = NULL;
  }
#endif

  if (s->null_thread_started) {
    pthread_t th = s->null_thread;
    s->null_thread_started = 0;
    pthread_mutex_unlock(&g_ao_mu);
    pthread_join(th, NULL);
    pthread_mutex_lock(&g_ao_mu);
  }

  pthread_mutex_destroy(&s->write_mu);
  if (s->ring) {
    free(s->ring);
    s->ring = NULL;
  }
  memset(s, 0, sizeof(*s));
  pthread_mutex_unlock(&g_ao_mu);
}

void fv_audio_out_close_all(void) {
  for (int32_t h = 1; h <= FV_AUDIO_OUT_MAX_SLOTS; h++) {
    fv_audio_out_close(h);
  }
}

int32_t fv_audio_out_active_count(void) {
  pthread_mutex_lock(&g_ao_mu);
  int32_t cnt = 0;
  for (int i = 0; i < FV_AUDIO_OUT_MAX_SLOTS; i++) {
    if (g_ao_slots[i].used) cnt++;
  }
  pthread_mutex_unlock(&g_ao_mu);
  return cnt;
}

int32_t fv_audio_out_get_stats(int32_t handle, FvAudioOutStats *out) {
  if (!out) return 0;
  FvAudioOutSlot *s = get_ao_slot(handle);
  if (!s) return 0;
  memset(out, 0, sizeof(*out));
  out->backend = s->backend;
  out->sample_rate = s->sample_rate;
  out->channels = s->channels;
  out->format = s->format;
  out->running = atomic_load_explicit(&s->running, memory_order_relaxed);
  out->muted = atomic_load_explicit(&s->muted, memory_order_relaxed);
  out->volume = atomic_load_explicit(&s->volume, memory_order_relaxed);
  out->buffered_bytes = (int32_t)ring_available(s);
  out->ring_capacity = (int32_t)FV_AUDIO_RING_CAP;
  out->frames_written = atomic_load_explicit(&s->frames_written, memory_order_relaxed);
  out->frames_played = atomic_load_explicit(&s->frames_played, memory_order_relaxed);
  out->callbacks = atomic_load_explicit(&s->callbacks, memory_order_relaxed);
  out->underruns = atomic_load_explicit(&s->underruns, memory_order_relaxed);
  out->overruns = atomic_load_explicit(&s->overruns, memory_order_relaxed);
  out->last_osstatus = s->last_osstatus;
  return 1;
}

int32_t fv_audio_out_stats_string(int32_t handle, char *out_buf, int32_t cap) {
  if (!out_buf || cap <= 1) return -1;
  FvAudioOutStats st;
  if (!fv_audio_out_get_stats(handle, &st)) return -1;
  const char *bname = (st.backend == FV_AUDIO_BACKEND_COREAUDIO) ? "coreaudio"
                    : (st.backend == FV_AUDIO_BACKEND_WINMM) ? "winmm"
                    : (st.backend == FV_AUDIO_BACKEND_NULL) ? "null" : "none";
  int n = snprintf(
      out_buf, (size_t)cap,
      "backend=%s sample_rate=%d channels=%d format=%s running=%d muted=%d volume=%.2f "
      "buffered_bytes=%d ring_capacity=%d frames_written=%lld frames_played=%lld "
      "callbacks=%lld underruns=%lld overruns=%lld osstatus=%d",
      bname, st.sample_rate, st.channels,
      st.format == FV_AUDIO_FMT_F32 ? "f32" : "s16",
      st.running, st.muted, (double)st.volume,
      st.buffered_bytes, st.ring_capacity,
      (long long)st.frames_written, (long long)st.frames_played,
      (long long)st.callbacks, (long long)st.underruns, (long long)st.overruns,
      st.last_osstatus);
  if (n < 0) return -1;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}
