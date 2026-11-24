#ifndef FV_PION_JS_MOONBIT_COMPAT_H
#define FV_PION_JS_MOONBIT_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int32_t len;
  int32_t rc;
  uint16_t data[];
} fv_mb_str_hdr_t;

typedef uint16_t *moonbit_string_t;

static inline moonbit_string_t moonbit_make_string(int32_t len, uint16_t fill) {
  int32_t n = len < 0 ? 0 : len;
  fv_mb_str_hdr_t *h = (fv_mb_str_hdr_t *)malloc(sizeof(fv_mb_str_hdr_t) + ((size_t)n + 1) * sizeof(uint16_t));
  h->len = n;
  h->rc = 1;
  for (int32_t i = 0; i < n; i++) h->data[i] = fill;
  h->data[n] = 0;
  return h->data;
}

static inline int32_t fv_mb_str_len(moonbit_string_t s) {
  if (!s) return 0;
  return ((fv_mb_str_hdr_t *)((uint8_t *)s - offsetof(fv_mb_str_hdr_t, data)))->len;
}

static inline void moonbit_decref(void *p) {
  if (!p) return;
  fv_mb_str_hdr_t *h = (fv_mb_str_hdr_t *)((uint8_t *)p - offsetof(fv_mb_str_hdr_t, data));
  if (--h->rc <= 0) {
    free(h);
  }
}

#define Moonbit_array_length(s) fv_mb_str_len(s)

#ifdef __cplusplus
}
#endif

#endif
