// Native helpers for HTML <video>/<audio> media probing, persistent non-blocking MJPEG streaming,
// in-memory single-frame JPEG extraction, and temp-dir / child-process cleanup.
// Uses fork()+execv() directly (no shell) so URLs/paths can never inject shell commands.
#include <moonbit.h>
#include "media_av.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include "../win_compat.h"

static char g_media_tmp_dir[512] = {0};
static int g_atexit_registered = 0;

static void rm_dir_contents(const char* dir) {
  if (!dir || !dir[0]) return;
  char pat[600];
  snprintf(pat, sizeof(pat), "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
      char path[600];
      snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        rm_dir_contents(path);
      } else {
        DeleteFileA(path);
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(dir);
}

static void media_atexit_cleanup(void) {
  fv_av_close_all_streams();
  fv_av_close_all_audio();
  if (g_media_tmp_dir[0]) {
    rm_dir_contents(g_media_tmp_dir);
    g_media_tmp_dir[0] = 0;
  }
}

static void ensure_atexit(void) {
  if (!g_atexit_registered) {
    g_atexit_registered = 1;
    atexit(media_atexit_cleanup);
  }
}

int32_t fv_media_pid(void) { return (int32_t)getpid(); }
int32_t fv_media_has_ffmpeg(void) { return fv_media_inproc_available() ? 1 : 0; }

void fv_media_register_dir(moonbit_bytes_t dir) {
  size_t n = (size_t)Moonbit_array_length(dir);
  if (n >= sizeof(g_media_tmp_dir)) n = sizeof(g_media_tmp_dir) - 1;
  memcpy(g_media_tmp_dir, dir, n);
  g_media_tmp_dir[n] = 0;
  ensure_atexit();
}

void fv_media_cleanup_dir(moonbit_bytes_t dir) {
  size_t n = (size_t)Moonbit_array_length(dir);
  if (n == 0 || n >= 512) return;
  char buf[512];
  memcpy(buf, dir, n);
  buf[n] = 0;
  rm_dir_contents(buf);
}

void fv_media_unlink(moonbit_bytes_t path) {
  size_t n = (size_t)Moonbit_array_length(path);
  if (n == 0 || n >= 512) return;
  char buf[512];
  memcpy(buf, path, n);
  buf[n] = 0;
  unlink(buf);
}

int32_t fv_media_append_file(moonbit_bytes_t path_bytes, moonbit_bytes_t data_bytes) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return 0;
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  int fd = _open(path, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
  if (fd < 0) return 0;
  size_t dlen = (size_t)Moonbit_array_length(data_bytes);
  const uint8_t* p = (const uint8_t*)data_bytes;
  size_t off = 0;
  int ok = 1;
  while (off < dlen) {
    int w = _write(fd, p + off, (unsigned int)(dlen - off));
    if (w < 0) {
      ok = 0;
      break;
    }
    off += (size_t)w;
  }
  _close(fd);
  return ok;
}

moonbit_bytes_t fv_media_probe(moonbit_bytes_t path_bytes) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return moonbit_make_bytes(0, 0);
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  if (fv_media_inproc_available()) {
    moonbit_bytes_t inproc = fv_av_probe_file(path);
    if (Moonbit_array_length(inproc) > 0) return inproc;
    fv_mb_bytes_free(inproc);
  }
  return moonbit_make_bytes(0, 0);
}

moonbit_bytes_t fv_media_frame_jpeg(moonbit_bytes_t path_bytes, int32_t time_ms, int32_t scale_w) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return moonbit_make_bytes(0, 0);
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  if (fv_media_inproc_available()) {
    moonbit_bytes_t inproc = fv_av_frame_jpeg_file(path, time_ms, scale_w);
    if (Moonbit_array_length(inproc) > 0) return inproc;
    fv_mb_bytes_free(inproc);
  }
  return moonbit_make_bytes(0, 0);
}

int32_t fv_media_stream_open(moonbit_bytes_t path_bytes, int32_t start_ms, int32_t scale_w, int32_t fps) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return -1;
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  if (fv_media_inproc_available()) {
    int32_t avh = fv_av_stream_open_file(path, start_ms, scale_w, fps);
    if (avh >= 0) {
      ensure_atexit();
      return avh;
    }
  }
  return -1;
}

moonbit_bytes_t fv_media_stream_read(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_stream_read_jpeg(handle);
  return moonbit_make_bytes(0, 0);
}

int32_t fv_media_stream_eof(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_stream_eof(handle);
  return 1;
}

void fv_media_stream_close(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) fv_av_stream_close(handle);
}

int32_t fv_media_stream_count(void) { return fv_av_stream_count(); }

int32_t fv_media_audio_open(moonbit_bytes_t path_bytes, int32_t start_ms) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return -1;
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  if (fv_media_inproc_available()) {
    int32_t avh = fv_av_audio_open_file(path, start_ms);
    if (avh >= 0) {
      ensure_atexit();
      return avh;
    }
  }
  return -1;
}

moonbit_bytes_t fv_media_audio_read(int32_t handle, int32_t max_bytes) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_audio_read(handle, max_bytes);
  return moonbit_make_bytes(0, 0);
}

int32_t fv_media_audio_eof(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_audio_eof(handle);
  return 1;
}

void fv_media_audio_close(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) fv_av_audio_close(handle);
}

int32_t fv_media_audio_count(void) { return fv_av_audio_count(); }

#else /* POSIX */

#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>

#define FV_MAX_MEDIA_STREAMS 16
#define FV_STREAM_MAX_BUF (4 * 1024 * 1024)

typedef struct {
  int in_use;
  pid_t pid;
  int fd;
  int eof;
  uint8_t* buf;
  size_t len;
  size_t cap;
} MediaStream;

static MediaStream g_streams[FV_MAX_MEDIA_STREAMS];
static MediaStream g_audio_streams[FV_MAX_MEDIA_STREAMS];
static char g_media_tmp_dir[256] = {0};
static int g_atexit_registered = 0;

static void close_audio_slot(int h) {
  if (h < 0 || h >= FV_MAX_MEDIA_STREAMS || !g_audio_streams[h].in_use) return;
  MediaStream* s = &g_audio_streams[h];
  if (s->fd >= 0) {
    close(s->fd);
    s->fd = -1;
  }
  if (s->pid > 0) {
    int status = 0;
    if (waitpid(s->pid, &status, WNOHANG) == 0) {
      kill(s->pid, SIGTERM);
      int reaped = 0;
      for (int i = 0; i < 50; i++) {
        if (waitpid(s->pid, &status, WNOHANG) != 0) {
          reaped = 1;
          break;
        }
        usleep(1000);
      }
      if (!reaped) {
        kill(s->pid, SIGKILL);
        waitpid(s->pid, &status, 0);
      }
    }
    s->pid = 0;
  }
  s->eof = 1;
  s->in_use = 0;
}

static void close_all_audio_streams(void) {
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (g_audio_streams[i].in_use) close_audio_slot(i);
  }
}

static void close_stream_slot(int h) {
  if (h < 0 || h >= FV_MAX_MEDIA_STREAMS || !g_streams[h].in_use) return;
  MediaStream* s = &g_streams[h];
  if (s->fd >= 0) {
    close(s->fd);
    s->fd = -1;
  }
  if (s->pid > 0) {
    int status = 0;
    if (waitpid(s->pid, &status, WNOHANG) == 0) {
      kill(s->pid, SIGTERM);
      int reaped = 0;
      for (int i = 0; i < 50; i++) {
        if (waitpid(s->pid, &status, WNOHANG) != 0) {
          reaped = 1;
          break;
        }
        usleep(1000);
      }
      if (!reaped) {
        kill(s->pid, SIGKILL);
        waitpid(s->pid, &status, 0);
      }
    }
    s->pid = 0;
  }
  if (s->buf) {
    free(s->buf);
    s->buf = NULL;
  }
  s->len = 0;
  s->cap = 0;
  s->eof = 1;
  s->in_use = 0;
}

static void close_all_streams(void) {
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (g_streams[i].in_use) close_stream_slot(i);
  }
}

static void rm_dir_contents(const char* dir) {
  if (!dir || !dir[0]) return;
  DIR* d = opendir(dir);
  if (d) {
    struct dirent* e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      struct stat st;
      if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        rm_dir_contents(path);
      } else {
        unlink(path);
      }
    }
    closedir(d);
  }
  rmdir(dir);
}

static void media_atexit_cleanup(void) {
  fv_av_close_all_streams();
  fv_av_close_all_audio();
  close_all_streams();
  close_all_audio_streams();
  if (g_media_tmp_dir[0]) {
    rm_dir_contents(g_media_tmp_dir);
    g_media_tmp_dir[0] = 0;
  }
}

static void ensure_atexit(void) {
  if (!g_atexit_registered) {
    g_atexit_registered = 1;
    atexit(media_atexit_cleanup);
  }
}

static int find_in_path(const char* bin) {
  const char* path = getenv("PATH");
  char buf[512];
  if (path) {
    const char* p = path;
    while (*p) {
      const char* colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len > 0 && len + strlen(bin) + 2 < sizeof(buf)) {
        memcpy(buf, p, len);
        buf[len] = '/';
        strcpy(buf + len + 1, bin);
        if (access(buf, X_OK) == 0) return 1;
      }
      if (!colon) break;
      p = colon + 1;
    }
  }
  const char* fallbacks[] = { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL };
  for (int i = 0; fallbacks[i]; i++) {
    snprintf(buf, sizeof(buf), "%s/%s", fallbacks[i], bin);
    if (access(buf, X_OK) == 0) return 1;
  }
  return 0;
}

static const char* resolve_bin(const char* bin, char* out, size_t cap) {
  const char* path = getenv("PATH");
  if (path) {
    const char* p = path;
    while (*p) {
      const char* colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len > 0 && len + strlen(bin) + 2 < cap) {
        memcpy(out, p, len);
        out[len] = '/';
        strcpy(out + len + 1, bin);
        if (access(out, X_OK) == 0) return out;
      }
      if (!colon) break;
      p = colon + 1;
    }
  }
  const char* fallbacks[] = { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL };
  for (int i = 0; fallbacks[i]; i++) {
    snprintf(out, cap, "%s/%s", fallbacks[i], bin);
    if (access(out, X_OK) == 0) return out;
  }
  return bin;
}

static int g_has_ffmpeg = -1;

int32_t fv_media_pid(void) {
  return (int32_t)getpid();
}

int32_t fv_media_has_ffmpeg(void) {
  if (fv_media_inproc_available()) return 1;
  if (g_has_ffmpeg < 0) {
    g_has_ffmpeg = (find_in_path("ffmpeg") && find_in_path("ffprobe")) ? 1 : 0;
  }
  return g_has_ffmpeg;
}

void fv_media_register_dir(moonbit_bytes_t dir) {
  size_t n = (size_t)Moonbit_array_length(dir);
  if (n >= sizeof(g_media_tmp_dir)) n = sizeof(g_media_tmp_dir) - 1;
  memcpy(g_media_tmp_dir, dir, n);
  g_media_tmp_dir[n] = 0;
  ensure_atexit();
}

void fv_media_cleanup_dir(moonbit_bytes_t dir) {
  size_t n = (size_t)Moonbit_array_length(dir);
  if (n == 0 || n >= 512) return;
  char buf[512];
  memcpy(buf, dir, n);
  buf[n] = 0;
  rm_dir_contents(buf);
}

void fv_media_unlink(moonbit_bytes_t path) {
  size_t n = (size_t)Moonbit_array_length(path);
  if (n == 0 || n >= 512) return;
  char buf[512];
  memcpy(buf, path, n);
  buf[n] = 0;
  unlink(buf);
}

int32_t fv_media_append_file(moonbit_bytes_t path_bytes, moonbit_bytes_t data_bytes) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return 0;
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return 0;
  size_t dlen = (size_t)Moonbit_array_length(data_bytes);
  const uint8_t* p = (const uint8_t*)data_bytes;
  size_t off = 0;
  int ok = 1;
  while (off < dlen) {
    ssize_t w = write(fd, p + off, dlen - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      ok = 0;
      break;
    }
    off += (size_t)w;
  }
  close(fd);
  return ok;
}

moonbit_bytes_t fv_media_probe(moonbit_bytes_t path_bytes) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return moonbit_make_bytes(0, 0);
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;

  if (fv_media_inproc_available()) {
    moonbit_bytes_t inproc = fv_av_probe_file(path);
    if (Moonbit_array_length(inproc) > 0) return inproc;
    fv_mb_bytes_free(inproc);
  }
  if (!find_in_path("ffprobe")) return moonbit_make_bytes(0, 0);

  int pfd[2];
  if (pipe(pfd) != 0) return moonbit_make_bytes(0, 0);
  char bin[512];
  const char* ffprobe = resolve_bin("ffprobe", bin, sizeof(bin));

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pfd[1]);
    char* argv[] = {
      (char*)ffprobe,
      "-v", "error",
      "-show_entries", "format=duration:stream=width,height,codec_type",
      "-of", "default=noprint_wrappers=1",
      path,
      NULL
    };
    execv(ffprobe, argv);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) {
    close(pfd[0]);
    return moonbit_make_bytes(0, 0);
  }
  char out[4096];
  size_t total = 0;
  while (total + 1 < sizeof(out)) {
    ssize_t r = read(pfd[0], out + total, sizeof(out) - 1 - total);
    if (r <= 0) break;
    total += (size_t)r;
  }
  close(pfd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)total, 0);
  if (total > 0) memcpy(res, out, total);
  return res;
}

// Single-frame JPEG extraction via pipe:1 (in-memory, zero temp files on disk).
moonbit_bytes_t fv_media_frame_jpeg(moonbit_bytes_t path_bytes, int32_t time_ms, int32_t scale_w) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return moonbit_make_bytes(0, 0);
  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;

  if (fv_media_inproc_available()) {
    moonbit_bytes_t inproc = fv_av_frame_jpeg_file(path, time_ms, scale_w);
    if (Moonbit_array_length(inproc) > 0) return inproc;
    fv_mb_bytes_free(inproc);
  }
  if (!find_in_path("ffmpeg")) return moonbit_make_bytes(0, 0);

  char ts[32];
  if (time_ms < 0) time_ms = 0;
  snprintf(ts, sizeof(ts), "%d.%03d", time_ms / 1000, time_ms % 1000);

  if (scale_w <= 0) scale_w = 640;
  char vf[96];
  snprintf(vf, sizeof(vf), "scale='min(%d,iw)':-2", scale_w);

  int pfd[2];
  if (pipe(pfd) != 0) return moonbit_make_bytes(0, 0);

  char bin[512];
  const char* ffmpeg = resolve_bin("ffmpeg", bin, sizeof(bin));

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pfd[1]);
    char* argv[] = {
      (char*)ffmpeg,
      "-v", "error",
      "-ss", ts,
      "-i", path,
      "-frames:v", "1",
      "-vf", vf,
      "-f", "image2pipe",
      "-vcodec", "mjpeg",
      "-q:v", "5",
      "pipe:1",
      NULL
    };
    execv(ffmpeg, argv);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) {
    close(pfd[0]);
    return moonbit_make_bytes(0, 0);
  }

  size_t cap = 65536;
  size_t len = 0;
  uint8_t* buf = (uint8_t*)malloc(cap);
  if (buf) {
    for (;;) {
      if (len + 16384 > cap) {
        if (cap >= FV_STREAM_MAX_BUF) break;
        size_t ncap = cap * 2;
        if (ncap > FV_STREAM_MAX_BUF) ncap = FV_STREAM_MAX_BUF;
        uint8_t* nbuf = (uint8_t*)realloc(buf, ncap);
        if (!nbuf) break;
        buf = nbuf;
        cap = ncap;
      }
      ssize_t r = read(pfd[0], buf + len, cap - len);
      if (r < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (r == 0) break;
      len += (size_t)r;
    }
  }
  close(pfd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)len, 0);
  if (len > 0 && buf) memcpy(res, buf, len);
  if (buf) free(buf);
  return res;
}

// Persistent non-blocking MJPEG stream: spawns one long-lived ffmpeg decoding at `fps` (default 24)
// to a non-blocking pipe; natural OS pipe backpressure paces ffmpeg without buffering stalls.
int32_t fv_media_stream_open(moonbit_bytes_t path_bytes, int32_t start_ms, int32_t scale_w, int32_t fps) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return -1;

  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;

  if (fv_media_inproc_available()) {
    int32_t avh = fv_av_stream_open_file(path, start_ms, scale_w, fps);
    if (avh >= 0) {
      ensure_atexit();
      return avh;
    }
  }
  if (!find_in_path("ffmpeg")) return -1;

  int slot = -1;
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (!g_streams[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -1;

  char ts[32];
  if (start_ms < 0) start_ms = 0;
  snprintf(ts, sizeof(ts), "%d.%03d", start_ms / 1000, start_ms % 1000);

  if (fps <= 0) fps = 24;
  if (scale_w <= 0) scale_w = 640;
  char vf[128];
  snprintf(vf, sizeof(vf), "fps=%d,scale='min(%d,iw)':-2", fps, scale_w);

  int pfd[2];
  if (pipe(pfd) != 0) return -1;

  char bin[512];
  const char* ffmpeg = resolve_bin("ffmpeg", bin, sizeof(bin));

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pfd[1]);
    char* argv[] = {
      (char*)ffmpeg,
      "-v", "error",
      "-ss", ts,
      "-i", path,
      "-vf", vf,
      "-f", "image2pipe",
      "-vcodec", "mjpeg",
      "-q:v", "5",
      "-flush_packets", "1",
      "pipe:1",
      NULL
    };
    execv(ffmpeg, argv);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) {
    close(pfd[0]);
    return -1;
  }

  int flags = fcntl(pfd[0], F_GETFL, 0);
  if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

  MediaStream* s = &g_streams[slot];
  s->in_use = 1;
  s->pid = pid;
  s->fd = pfd[0];
  s->eof = 0;
  s->cap = 131072;
  s->buf = (uint8_t*)malloc(s->cap);
  s->len = 0;
  if (!s->buf) {
    close_stream_slot(slot);
    return -1;
  }
  ensure_atexit();
  return (int32_t)slot;
}

static int find_first_jpeg(const uint8_t* buf, size_t len, size_t* out_soi, size_t* out_eoi) {
  size_t soi = 0;
  while (soi + 1 < len && !(buf[soi] == 0xFF && buf[soi + 1] == 0xD8)) {
    soi++;
  }
  if (soi + 1 >= len) return 0;
  size_t j = soi + 2;
  while (j + 1 < len) {
    if (buf[j] == 0xFF && buf[j + 1] == 0xD9) {
      *out_soi = soi;
      *out_eoi = j + 2;
      return 1;
    }
    j++;
  }
  return 0;
}

// Non-blocking read of the next single complete JPEG frame (0xFF 0xD8 ... 0xFF 0xD9) from the
// persistent MJPEG stream; returns empty Bytes if none ready yet.
moonbit_bytes_t fv_media_stream_read(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_stream_read_jpeg(handle);
  if (handle < 0 || handle >= FV_MAX_MEDIA_STREAMS || !g_streams[handle].in_use) {
    return moonbit_make_bytes(0, 0);
  }
  MediaStream* s = &g_streams[handle];
  size_t soi = 0, eoi = 0;
  if (!find_first_jpeg(s->buf, s->len, &soi, &eoi) && s->fd >= 0 && !s->eof) {
    size_t read_budget = 262144;
    while (read_budget > 0) {
      if (s->len + 32768 > s->cap) {
        if (s->cap >= FV_STREAM_MAX_BUF) {
          s->len = 0;
        } else {
          size_t ncap = s->cap * 2;
          if (ncap > FV_STREAM_MAX_BUF) ncap = FV_STREAM_MAX_BUF;
          uint8_t* nbuf = (uint8_t*)realloc(s->buf, ncap);
          if (!nbuf) break;
          s->buf = nbuf;
          s->cap = ncap;
        }
      }
      size_t want = s->cap - s->len;
      if (want > read_budget) want = read_budget;
      ssize_t r = read(s->fd, s->buf + s->len, want);
      if (r > 0) {
        s->len += (size_t)r;
        read_budget -= (size_t)r;
        if (find_first_jpeg(s->buf, s->len, &soi, &eoi)) break;
      } else if (r == 0) {
        s->eof = 1;
        break;
      } else {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        s->eof = 1;
        break;
      }
    }
  }

  if (eoi > soi) {
    size_t flen = eoi - soi;
    moonbit_bytes_t res = moonbit_make_bytes((int32_t)flen, 0);
    memcpy(res, s->buf + soi, flen);
    size_t rem = s->len - eoi;
    if (rem > 0) {
      memmove(s->buf, s->buf + eoi, rem);
    }
    s->len = rem;
    return res;
  }
  return moonbit_make_bytes(0, 0);
}

int32_t fv_media_stream_eof(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_stream_eof(handle);
  if (handle < 0 || handle >= FV_MAX_MEDIA_STREAMS || !g_streams[handle].in_use) return 1;
  return g_streams[handle].eof ? 1 : 0;
}

void fv_media_stream_close(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) { fv_av_stream_close(handle); return; }
  close_stream_slot(handle);
}

int32_t fv_media_stream_count(void) {
  int n = fv_av_stream_count();
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (g_streams[i].in_use) n++;
  }
  return n;
}

// Persistent non-blocking 48 kHz stereo s16le PCM audio stream (in-process FFmpeg or CLI fallback).
int32_t fv_media_audio_open(moonbit_bytes_t path_bytes, int32_t start_ms) {
  size_t plen = (size_t)Moonbit_array_length(path_bytes);
  if (plen == 0 || plen >= 512) return -1;

  char path[512];
  memcpy(path, path_bytes, plen);
  path[plen] = 0;

  if (fv_media_inproc_available()) {
    int32_t avh = fv_av_audio_open_file(path, start_ms);
    if (avh >= 0) {
      ensure_atexit();
      return avh;
    }
  }
  if (!find_in_path("ffmpeg")) return -1;

  int slot = -1;
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (!g_audio_streams[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -1;

  char ts[32];
  if (start_ms < 0) start_ms = 0;
  snprintf(ts, sizeof(ts), "%d.%03d", start_ms / 1000, start_ms % 1000);

  int pfd[2];
  if (pipe(pfd) != 0) return -1;

  char bin[512];
  const char* ffmpeg = resolve_bin("ffmpeg", bin, sizeof(bin));

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pfd[1]);
    char* argv[] = {
      (char*)ffmpeg,
      "-nostdin",
      "-loglevel", "error",
      "-ss", ts,
      "-i", path,
      "-vn",
      "-f", "s16le",
      "-ac", "2",
      "-ar", "48000",
      "pipe:1",
      NULL
    };
    execv(ffmpeg, argv);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) {
    close(pfd[0]);
    return -1;
  }

  int flags = fcntl(pfd[0], F_GETFL, 0);
  if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

  MediaStream* s = &g_audio_streams[slot];
  s->in_use = 1;
  s->pid = pid;
  s->fd = pfd[0];
  s->eof = 0;
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
  ensure_atexit();
  return (int32_t)slot;
}

moonbit_bytes_t fv_media_audio_read(int32_t handle, int32_t max_bytes) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_audio_read(handle, max_bytes);
  if (handle < 0 || handle >= FV_MAX_MEDIA_STREAMS || !g_audio_streams[handle].in_use) {
    return moonbit_make_bytes(0, 0);
  }
  MediaStream* s = &g_audio_streams[handle];
  if (s->fd < 0 || s->eof || max_bytes <= 0) {
    return moonbit_make_bytes(0, 0);
  }
  if (max_bytes > 65536) max_bytes = 65536;
  uint8_t* tmp = (uint8_t*)malloc((size_t)max_bytes);
  if (!tmp) return moonbit_make_bytes(0, 0);
  size_t total = 0;
  while (total < (size_t)max_bytes) {
    ssize_t r = read(s->fd, tmp + total, (size_t)max_bytes - total);
    if (r > 0) {
      total += (size_t)r;
    } else if (r == 0) {
      s->eof = 1;
      break;
    } else {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      s->eof = 1;
      break;
    }
  }
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)total, 0);
  if (total > 0) memcpy(res, tmp, total);
  free(tmp);
  return res;
}

int32_t fv_media_audio_eof(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) return fv_av_audio_eof(handle);
  if (handle < 0 || handle >= FV_MAX_MEDIA_STREAMS || !g_audio_streams[handle].in_use) return 1;
  return g_audio_streams[handle].eof ? 1 : 0;
}

void fv_media_audio_close(int32_t handle) {
  if (handle >= FV_AV_HANDLE_BASE) { fv_av_audio_close(handle); return; }
  close_audio_slot(handle);
}

int32_t fv_media_audio_count(void) {
  int n = fv_av_audio_count();
  for (int i = 0; i < FV_MAX_MEDIA_STREAMS; i++) {
    if (g_audio_streams[i].in_use) n++;
  }
  return n;
}
#endif /* _WIN32 */
