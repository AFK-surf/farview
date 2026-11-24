#include "c_compat/moonbit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <CoreText/CoreText.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

extern moonbit_bytes_t fv_ct_fallback(moonbit_bytes_t family, int32_t c, int32_t weight);
extern moonbit_bytes_t fv_ct_outline(moonbit_bytes_t ps, int32_t glyph, int32_t upem, int32_t wght);

int32_t pion_hf_mb_len(void *b) {
  return fv_mb_bytes_len((moonbit_bytes_t)b);
}

void pion_hf_mb_free(void *b) {
  fv_mb_bytes_free((moonbit_bytes_t)b);
}

void *pion_hf_ct_fallback(const void *family_cstr, int32_t c, int32_t weight) {
  return (void *)fv_ct_fallback((moonbit_bytes_t)family_cstr, c, weight);
}

void *pion_hf_ct_outline(const void *ps_cstr, int32_t glyph, int32_t upem, int32_t wght) {
  return (void *)fv_ct_outline((moonbit_bytes_t)ps_cstr, glyph, upem, wght);
}

int32_t pion_hf_file_exists(const void *path_cstr) {
  struct stat st;
  return stat((const char *)path_cstr, &st) == 0 ? 1 : 0;
}

int32_t pion_hf_file_size(const void *path_cstr) {
  struct stat st;
  if (stat((const char *)path_cstr, &st) != 0 || st.st_size <= 0 || st.st_size > 0x7FFFFFFF) {
    return 0;
  }
  return (int32_t)st.st_size;
}

int32_t pion_hf_read_into(const void *path_cstr, void *dst, int32_t max_len) {
  if (!dst || max_len <= 0) return 0;
  FILE *f = fopen((const char *)path_cstr, "rb");
  if (!f) return 0;
  size_t got = fread(dst, 1, (size_t)max_len, f);
  fclose(f);
  return (int32_t)got;
}

void *pion_hf_read_file_bytes(const void *path_cstr) {
  FILE *f = fopen((const char *)path_cstr, "rb");
  if (!f) return (void *)moonbit_make_bytes(0, 0);
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return (void *)moonbit_make_bytes(0, 0);
  }
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return (void *)moonbit_make_bytes(0, 0);
  }
  rewind(f);
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)sz, 0);
  size_t got = fread(out, 1, (size_t)sz, f);
  fclose(f);
  ((fv_mb_bytes_hdr_t *)(out - offsetof(fv_mb_bytes_hdr_t, data)))->len = (int64_t)got;
  return (void *)out;
}

#ifdef __APPLE__
static CTFontRef pion_ct_open_font(const char *family, double size, int32_t weight) {
  if (!family || family[0] == 0) {
    return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, (CGFloat)size, NULL);
  }
  CFStringRef name = CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);
  if (!name) return NULL;
  double w = weight <= 400 ? (weight - 400) / 300.0 * 0.4 : (weight - 400) / 500.0 * 0.62;
  CFNumberRef wn = CFNumberCreate(NULL, kCFNumberDoubleType, &w);
  const void *tk[] = {kCTFontWeightTrait};
  const void *tv[] = {wn};
  CFDictionaryRef traits = CFDictionaryCreate(
      NULL, tk, tv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const void *ak[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
  const void *av[] = {name, traits};
  CFDictionaryRef attrs = CFDictionaryCreate(
      NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
  CTFontRef f = CTFontCreateWithFontDescriptor(d, (CGFloat)size, NULL);
  CFRelease(d);
  CFRelease(attrs);
  CFRelease(traits);
  CFRelease(wn);
  CFRelease(name);
  return f;
}
#endif

// Writes [ascent, descent, line_gap, x_height] into out4 (4 doubles). Returns 1 on success.
int32_t pion_hf_ct_metrics(const void *family_cstr, double size, int32_t weight, void *out4) {
  double *dst = (double *)out4;
  dst[0] = size * 0.8;
  dst[1] = size * 0.2;
  dst[2] = 0.0;
  dst[3] = size * 0.5;
#ifdef __APPLE__
  CTFontRef f = pion_ct_open_font((const char *)family_cstr, size, weight);
  if (!f) return 0;
  dst[0] = (double)CTFontGetAscent(f);
  dst[1] = (double)CTFontGetDescent(f);
  dst[2] = (double)CTFontGetLeading(f);
  dst[3] = (double)CTFontGetXHeight(f);
  CFRelease(f);
  return 1;
#else
  (void)family_cstr;
  (void)weight;
  return 0;
#endif
}

// Measures UTF-8 text width using CoreText (CTLineCreateWithAttributedString) including system fallback.
double pion_hf_ct_measure_text(const void *family_cstr, double size, int32_t weight, const void *text_cstr) {
#ifdef __APPLE__
  CTFontRef f = pion_ct_open_font((const char *)family_cstr, size, weight);
  if (!f) return 0.0;
  CFStringRef s = CFStringCreateWithCString(NULL, (const char *)text_cstr, kCFStringEncodingUTF8);
  if (!s) {
    CFRelease(f);
    return 0.0;
  }
  const void *keys[] = {kCTFontAttributeName};
  const void *vals[] = {f};
  CFDictionaryRef attrs = CFDictionaryCreate(
      NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef astr = CFAttributedStringCreate(NULL, s, attrs);
  CTLineRef line = CTLineCreateWithAttributedString(astr);
  double width = (double)CTLineGetTypographicBounds(line, NULL, NULL, NULL);
  CFRelease(line);
  CFRelease(astr);
  CFRelease(attrs);
  CFRelease(s);
  CFRelease(f);
  return width;
#else
  (void)family_cstr;
  (void)size;
  (void)weight;
  (void)text_cstr;
  return 0.0;
#endif
}

// Single codepoint advance via CoreText (with fallback via CTFontCreateForString).
double pion_hf_ct_glyph_advance(const void *family_cstr, double size, int32_t weight, int32_t cp) {
#ifdef __APPLE__
  CTFontRef base = pion_ct_open_font((const char *)family_cstr, size, weight);
  if (!base) return 0.0;
  UniChar u[2];
  CFIndex n = 1;
  if (cp >= 0x10000) {
    u[0] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10));
    u[1] = (UniChar)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    n = 2;
  } else {
    u[0] = (UniChar)cp;
  }
  CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
  CTFontRef f = CTFontCreateForString(base, s, CFRangeMake(0, n));
  double adv = 0.0;
  if (f) {
    CGGlyph glyphs[2] = {0, 0};
    if (CTFontGetGlyphsForCharacters(f, u, glyphs, n) && glyphs[0] != 0) {
      CGSize sz = CGSizeMake(0, 0);
      adv = (double)CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &glyphs[0], &sz, 1);
    }
    if (adv <= 0.0) {
      const void *keys[] = {kCTFontAttributeName};
      const void *vals[] = {f};
      CFDictionaryRef attrs = CFDictionaryCreate(
          NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFAttributedStringRef astr = CFAttributedStringCreate(NULL, s, attrs);
      CTLineRef line = CTLineCreateWithAttributedString(astr);
      adv = (double)CTLineGetTypographicBounds(line, NULL, NULL, NULL);
      CFRelease(line);
      CFRelease(astr);
      CFRelease(attrs);
    }
    CFRelease(f);
  }
  CFRelease(s);
  CFRelease(base);
  return adv;
#else
  (void)family_cstr;
  (void)size;
  (void)weight;
  (void)cp;
  return 0.0;
#endif
}
