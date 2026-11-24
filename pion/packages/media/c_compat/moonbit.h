#ifndef FV_PION_MEDIA_MOONBIT_COMPAT_H
#define FV_PION_MEDIA_MOONBIT_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t len;
  uint8_t data[];
} fv_mb_bytes_hdr_t;

typedef uint8_t *moonbit_bytes_t;

static inline moonbit_bytes_t moonbit_make_bytes(int32_t len, int32_t fill) {
  int32_t n = len < 0 ? 0 : len;
  fv_mb_bytes_hdr_t *h = (fv_mb_bytes_hdr_t *)malloc(sizeof(fv_mb_bytes_hdr_t) + (size_t)n + 1);
  h->len = n;
  if (n > 0) memset(h->data, fill, (size_t)n);
  h->data[n] = 0;
  return h->data;
}

static inline int32_t fv_mb_bytes_len(moonbit_bytes_t b) {
  if (!b) return 0;
  return (int32_t)((fv_mb_bytes_hdr_t *)(b - offsetof(fv_mb_bytes_hdr_t, data)))->len;
}

static inline void fv_mb_bytes_free(moonbit_bytes_t b) {
  if (b) free(b - offsetof(fv_mb_bytes_hdr_t, data));
}

#define Moonbit_array_length(b) fv_mb_bytes_len(b)

#endif
