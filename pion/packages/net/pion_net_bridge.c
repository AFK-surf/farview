#include "pion_rt.h"
#include <stdint.h>
#ifdef _WIN32
#include "../../../native/win_compat.h"

typedef struct {
  char d_name[MAX_PATH];
} FvWinDirent;

typedef struct {
  HANDLE h;
  WIN32_FIND_DATAA fd;
  int first;
  FvWinDirent ent;
} FvWinDir;

void *opendir(const char *path) {
  if (!path || !path[0]) return NULL;
  char pat[4096];
  size_t len = strlen(path);
  if (len + 4 >= sizeof(pat)) return NULL;
  memcpy(pat, path, len);
  if (len > 0 && pat[len - 1] != '/' && pat[len - 1] != '\\') pat[len++] = '\\';
  pat[len++] = '*';
  pat[len] = '\0';
  FvWinDir *d = (FvWinDir *)malloc(sizeof(FvWinDir));
  if (!d) return NULL;
  d->h = FindFirstFileA(pat, &d->fd);
  if (d->h == INVALID_HANDLE_VALUE) {
    free(d);
    return NULL;
  }
  d->first = 1;
  return (void *)d;
}

void *readdir(void *dir) {
  FvWinDir *d = (FvWinDir *)dir;
  if (!d) return NULL;
  if (d->first) {
    d->first = 0;
  } else {
    if (!FindNextFileA(d->h, &d->fd)) return NULL;
  }
  snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", d->fd.cFileName);
  return (void *)&d->ent;
}

int closedir(void *dir) {
  FvWinDir *d = (FvWinDir *)dir;
  if (!d) return -1;
  if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  free(d);
  return 0;
}

const char *pion_dirent_name(void *ent) {
  return ent ? ((FvWinDirent *)ent)->d_name : "";
}
#else
#include <dirent.h>

const char *pion_dirent_name(void *ent) {
  return ent ? ((struct dirent *)ent)->d_name : "";
}
#endif

extern int32_t fv_poll(int32_t *fds, int32_t n, int32_t wait_ms);
extern int32_t fv_pipe_open(int32_t *out_fds);
extern int32_t fv_poll_fds(int32_t *fds, int32_t *events, int32_t *revents, int32_t n, int32_t wait_ms);

int32_t pion_fv_poll(void *fds_i32, int32_t n, int32_t wait_ms) {
  int32_t tmp[64];
  if (n > 64) n = 64;
  if (n < 0) n = 0;
  const int32_t *src = (const int32_t *)fds_i32;
  for (int32_t i = 0; i < n; i++) {
    tmp[i] = src[i];
  }
  return fv_poll(tmp, n, wait_ms);
}

int32_t pion_fv_pipe_open(void *out_i32) {
  return fv_pipe_open((int32_t *)out_i32);
}

int32_t pion_fv_poll_fds(void *fds_i32, void *events_i32, void *revents_i32, int32_t n, int32_t wait_ms) {
  return fv_poll_fds((int32_t *)fds_i32, (int32_t *)events_i32, (int32_t *)revents_i32, n, wait_ms);
}
