// Farview native macOS AVFoundation / CoreAudio / CoreGraphics capture & device enumeration.
// 100% pure C + dlopen + Objective-C Runtime (0 .m files, 0 hard-linked framework deps).
// Cleanly compiles to portable fallback stubs on Linux.
#ifndef FV_AV_MACOS_H
#define FV_AV_MACOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture backend modes */
#define FV_CAP_MODE_FALLBACK    0
#define FV_CAP_MODE_AVFOUNDATION 1
#define FV_CAP_MODE_COREGRAPHICS 2

/* Explicit reason codes when hardware capture is unavailable or degraded */
#define FV_CAP_REASON_OK                 0
#define FV_CAP_REASON_UNSUPPORTED_OS     1
#define FV_CAP_REASON_FRAMEWORK_MISSING  2
#define FV_CAP_REASON_NO_DEVICE          3
#define FV_CAP_REASON_PERMISSION_DENIED  4
#define FV_CAP_REASON_TCC_NOT_DETERMINED 5
#define FV_CAP_REASON_SESSION_ERROR      6
#define FV_CAP_REASON_FORCED_FALLBACK    7

/* Capture open flags */
#define FV_CAP_FLAG_ALLOW_FALLBACK 0
#define FV_CAP_FLAG_STRICT         1 /* Fail with negative error code instead of falling back */

typedef struct {
  int32_t avfoundation_loaded;
  int32_t coreaudio_loaded;
  int32_t coregraphics_loaded;
  int32_t screencapturekit_loaded;
  int32_t real_cameras;
  int32_t real_mics;
  int32_t real_speakers;
  int32_t cam_auth_status;    /* macOS AVAuthorizationStatus: 0=notDetermined,1=restricted,2=denied,3=authorized */
  int32_t mic_auth_status;
  int32_t screen_tcc_granted; /* CGPreflightScreenCaptureAccess() */
} FvAvDeviceSummary;

typedef struct FvMacCapture FvMacCapture;

/* Enumerate real OS cameras (AVFoundation), microphones (AVFoundation), and speakers (CoreAudio).
 * Writes tab-separated lines: "<kind>\t<deviceId>\t<label>\t<groupId>\n"
 * Explicitly marks virtual/fallback entries when physical hardware is absent.
 * Returns bytes written or -1. */
int32_t fv_av_macos_enumerate_devices(char *out_buf, int32_t cap, FvAvDeviceSummary *out_summary);

/* Open a real hardware capture backend for kind (0=audio, 1=video, 2=screen).
 * On success returns non-NULL handle and sets *out_mode / *out_reason.
 * If hardware is unavailable/unauthorized, returns NULL and sets *out_reason explicitly. */
FvMacCapture *fv_av_macos_capture_open(int32_t kind, int32_t width, int32_t height,
                                       int32_t *out_mode, int32_t *out_reason);

/* Read a real frame (RGBA8888 for video/screen, 48kHz mono s16le for audio).
 * Returns >0 bytes read, 0 if no hardware frame arrived yet, or -1 on fatal error. */
int32_t fv_av_macos_capture_read_frame(FvMacCapture *cap, uint8_t *out_buf, int32_t buf_cap);

/* Close and release all AVFoundation / CoreGraphics / GCD resources. */
void fv_av_macos_capture_close(FvMacCapture *cap);

/* Human-readable reason / mode strings */
const char *fv_cap_mode_name(int32_t mode);
const char *fv_cap_reason_name(int32_t reason);

#ifdef __cplusplus
}
#endif

#endif /* FV_AV_MACOS_H */
