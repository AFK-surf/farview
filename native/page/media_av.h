// Internal header for Farview in-process dlopen FFmpeg media engine (Wave M1).
#ifndef FV_MEDIA_AV_H
#define FV_MEDIA_AV_H

#include <moonbit.h>
#include <stdint.h>
#include <stddef.h>

// Returns 1 if libavformat/libavcodec/libswscale/libswresample/libavutil were
// successfully loaded via dlopen with a verified ABI; 0 otherwise (fallback to ffmpeg CLI).
int32_t fv_media_inproc_available(void);

// In-process probe from file path or raw memory bytes.
// Returns key=value lines compatible with ffprobe (-of default=noprint_wrappers=1),
// plus video_codec, audio_codec, sample_rate, channels, fps, buffered_start_ms, buffered_end_ms.
moonbit_bytes_t fv_av_probe_file(const char *path);
moonbit_bytes_t fv_media_probe_mem(moonbit_bytes_t data_bytes);

// In-process single-frame JPEG extraction from file path or memory bytes.
moonbit_bytes_t fv_av_frame_jpeg_file(const char *path, int32_t time_ms, int32_t scale_w);
moonbit_bytes_t fv_media_frame_jpeg_mem(moonbit_bytes_t data_bytes, int32_t time_ms, int32_t scale_w);

// In-process video stream (file-backed or memory-backed for MSE).
// Handles returned by fv_av_stream_open* live in the range [FV_AV_HANDLE_BASE, ...).
#define FV_AV_HANDLE_BASE 1000

int32_t fv_av_stream_open_file(const char *path, int32_t start_ms, int32_t scale_w, int32_t fps);
int32_t fv_media_mem_stream_open(int32_t scale_w, int32_t fps);
int32_t fv_media_mem_stream_append(int32_t handle, moonbit_bytes_t data_bytes);
void    fv_media_mem_stream_end(int32_t handle);
moonbit_bytes_t fv_media_mem_stream_probe(int32_t handle);

moonbit_bytes_t fv_av_stream_read_jpeg(int32_t handle);
// Returns [u32_le w][u32_le h][i64_le pts_ms][RGBA bytes (w*h*4)], or empty if none/EOF.
moonbit_bytes_t fv_media_stream_read_rgba(int32_t handle);
int32_t fv_media_stream_last_pts_ms(int32_t handle);
int32_t fv_media_stream_seek(int32_t handle, int32_t target_ms);
int32_t fv_av_stream_eof(int32_t handle);
void    fv_av_stream_close(int32_t handle);
int32_t fv_av_stream_count(void);
void    fv_av_close_all_streams(void);

// In-process audio stream (48 kHz stereo s16le PCM, file-backed or memory-backed for MSE).
int32_t fv_av_audio_open_file(const char *path, int32_t start_ms);
int32_t fv_media_mem_audio_open(void);
int32_t fv_media_mem_audio_append(int32_t handle, moonbit_bytes_t data_bytes);
void    fv_media_mem_audio_end(int32_t handle);
moonbit_bytes_t fv_av_audio_read(int32_t handle, int32_t max_bytes);
int32_t fv_media_audio_last_pts_ms(int32_t handle);
int32_t fv_media_audio_seek(int32_t handle, int32_t target_ms);
int32_t fv_av_audio_eof(int32_t handle);
void    fv_av_audio_close(int32_t handle);
int32_t fv_av_audio_count(void);
void    fv_av_close_all_audio(void);

// Real AudioContext.decodeAudioData backend:
// Decodes arbitrary audio bytes (MP3/AAC/WAV/OGG/Opus/FLAC/MP4/WebM) in-memory to
// [u32_le 0x46564144][u32_le sample_rate][u32_le channels][u32_le frames][planar float32 samples].
// Returns empty Bytes on decode failure.
moonbit_bytes_t fv_media_decode_audio(moonbit_bytes_t data_bytes);

// Attach an active fv_audio_out handle to an in-process audio decoder stream so
// decoded 48 kHz stereo s16le PCM frames are automatically fed to the speaker output.
int32_t fv_media_audio_attach_output(int32_t audio_handle, int32_t out_handle);

// Feed an FVAD planar float32 buffer (from fv_media_decode_audio) directly into an audio_out handle.
// Returns frames written, or -1 on invalid buffer/handle.
int32_t fv_media_play_decoded_audio(moonbit_bytes_t fvad_bytes, int32_t out_handle);

#endif
