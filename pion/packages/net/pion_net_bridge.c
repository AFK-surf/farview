#include "pion_rt.h"
#include <stdint.h>
#include <dirent.h>

extern int32_t fv_poll(int32_t *fds, int32_t n, int32_t wait_ms);

const char *pion_dirent_name(void *ent) {
  return ent ? ((struct dirent *)ent)->d_name : "";
}

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
