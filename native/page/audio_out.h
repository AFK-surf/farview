// Farview native real-time PCM audio output engine.
// macOS: dlopen AudioToolbox.framework (AudioQueue), 0 hard-linked framework dependencies.
// Linux / FV_AUDIO_NULL=1: silent null-sink consumer exercising the same lock-free ring buffer.
#ifndef FV_AUDIO_OUT_H
#define FV_AUDIO_OUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FV_AUDIO_FMT_S16 0 /* Interleaved signed 16-bit little-endian PCM */
#define FV_AUDIO_FMT_F32 1 /* Interleaved 32-bit IEEE float PCM */

#define FV_AUDIO_OUT_FLAG_NONE 0
#define FV_AUDIO_OUT_FLAG_NULL 1 /* Force silent null-sink mode (no speaker noise) */

#define FV_AUDIO_BACKEND_NONE      0
#define FV_AUDIO_BACKEND_COREAUDIO 1
#define FV_AUDIO_BACKEND_NULL      2
#define FV_AUDIO_BACKEND_WINMM     3

typedef struct {
  int32_t  backend;        /* FV_AUDIO_BACKEND_* */
  int32_t  sample_rate;
  int32_t  channels;
  int32_t  format;         /* FV_AUDIO_FMT_* */
  int32_t  running;        /* 1 if playing, 0 if paused/stopped */
  int32_t  muted;          /* 1 if muted */
  float    volume;         /* 0.0 .. 1.0 */
  int32_t  buffered_bytes; /* Current bytes queued in ring buffer */
  int32_t  ring_capacity;  /* Total ring buffer capacity in bytes */
  int64_t  frames_written; /* Total PCM frames pushed by producers */
  int64_t  frames_played;  /* Total PCM frames consumed by AudioQueue/null callback */
  int64_t  callbacks;      /* Number of audio callback invocations */
  int64_t  underruns;      /* Callbacks with insufficient ring data */
  int64_t  overruns;       /* Write calls that dropped/truncated due to full ring */
  int32_t  last_osstatus;  /* Last CoreAudio OSStatus (0 == noErr) */
} FvAudioOutStats;

/* Probe whether macOS AudioToolbox AudioQueueNewOutput can open a hardware queue
 * (allocates and immediately disposes without starting audible playback).
 * Returns 1 if hardware AudioQueue is ready, 0 otherwise. */
int32_t fv_audio_out_probe_hardware(void);

/* Open a new audio output stream (48000 / 44100 Hz, 1..2 ch, S16 or F32).
 * Returns handle 1..16 on success, -1 on failure. */
int32_t fv_audio_out_open(int32_t sample_rate, int32_t channels, int32_t fmt, int32_t flags);

/* Write interleaved PCM bytes (S16 or F32 matching stream format) to the ring buffer.
 * Returns number of bytes accepted (frame-aligned), or -1 on invalid handle. */
int32_t fv_audio_out_write(int32_t handle, const uint8_t *data, int32_t len);

/* Write planar float32 PCM (e.g. from AudioContext.decodeAudioData) into an audio output.
 * Automatically interleaves and converts to S16 if the output stream is FV_AUDIO_FMT_S16.
 * Returns number of frames accepted, or -1 on error. */
int32_t fv_audio_out_write_f32_planar(int32_t handle, const float *ch0, const float *ch1,
                                      int32_t frames);

/* Playback & volume controls */
int32_t fv_audio_out_play(int32_t handle);
int32_t fv_audio_out_pause(int32_t handle);
int32_t fv_audio_out_set_volume(int32_t handle, float volume);
int32_t fv_audio_out_set_muted(int32_t handle, int32_t muted);
int32_t fv_audio_out_flush(int32_t handle);

/* Close one or all output streams */
void fv_audio_out_close(int32_t handle);
void fv_audio_out_close_all(void);
int32_t fv_audio_out_active_count(void);

/* Query live stats; returns 1 on success, 0 on invalid handle. */
int32_t fv_audio_out_get_stats(int32_t handle, FvAudioOutStats *out);

/* Format live stats as a tab/key=value summary string into out_buf. Returns length or -1. */
int32_t fv_audio_out_stats_string(int32_t handle, char *out_buf, int32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FV_AUDIO_OUT_H */
