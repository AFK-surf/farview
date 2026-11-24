#include "c_compat/moonbit.h"
#include <stdint.h>
#include <string.h>

extern int32_t fv_pdf_page_count(moonbit_bytes_t pdf_bytes);
extern moonbit_bytes_t fv_pdf_title(moonbit_bytes_t pdf_bytes);
extern moonbit_bytes_t fv_pdf_render_page(moonbit_bytes_t pdf_bytes, int32_t page_1based, int32_t dpi);

static moonbit_bytes_t wrap_bytes(const void *p, int32_t len) {
  moonbit_bytes_t b = moonbit_make_bytes(len, 0);
  if (len > 0 && p) memcpy(b, p, (size_t)len);
  return b;
}

int32_t pion_pdf_mb_len(void *b) {
  return fv_mb_bytes_len((moonbit_bytes_t)b);
}

void pion_pdf_mb_free(void *b) {
  fv_mb_bytes_free((moonbit_bytes_t)b);
}

int32_t pion_pdf_page_count(const void *data, int32_t len) {
  moonbit_bytes_t mb = wrap_bytes(data, len);
  int32_t n = fv_pdf_page_count(mb);
  fv_mb_bytes_free(mb);
  return n;
}

void *pion_pdf_title(const void *data, int32_t len) {
  moonbit_bytes_t mb = wrap_bytes(data, len);
  moonbit_bytes_t out = fv_pdf_title(mb);
  fv_mb_bytes_free(mb);
  return (void *)out;
}

void *pion_pdf_render_page(const void *data, int32_t len, int32_t page_1based, int32_t dpi) {
  moonbit_bytes_t mb = wrap_bytes(data, len);
  moonbit_bytes_t out = fv_pdf_render_page(mb, page_1based, dpi);
  fv_mb_bytes_free(mb);
  return (void *)out;
}

int32_t pion_pdf_is_macos(void) {
#ifdef __APPLE__
  return 1;
#else
  return 0;
#endif
}
