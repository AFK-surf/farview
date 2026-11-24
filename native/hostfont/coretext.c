// The face macOS's CoreText puts in for a character the chosen face lacks: Chrome on macOS takes its fallback faces
// from CoreText (CTFontCreateForString), so asking it gives the same faces.
#include <moonbit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <CoreText/CoreText.h>
// The frameworks it needs, said in its object (Mach-O's LC_LINKER_OPTION): whatever program links this package, its
// main or a test's, links them, with no link flags of the package's own (a package with them is linked as a program
// of its own by `moon build`).
__asm__(".linker_option \"-framework\", \"CoreText\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n"
        ".linker_option \"-framework\", \"CoreGraphics\"\n");
#endif

// `family` (NUL-terminated UTF-8; "" for the system UI font) at `weight`, character `c`: "path\tPostScript name" of
// the face CoreText draws it with, or "" when it gives none or the face itself.
moonbit_bytes_t fv_ct_fallback(moonbit_bytes_t family, int32_t c, int32_t weight) {
  char out[2048] = "";
#ifndef __APPLE__
  (void)family;
  (void)c;
  (void)weight;
#else
  CTFontRef base = NULL;
  if (family[0] == 0) {
    base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 16.0, NULL);
  } else {
    CFStringRef name = CFStringCreateWithCString(NULL, (const char*)family, kCFStringEncodingUTF8);
    if (name) {
      // the family at the weight asked for (CoreText's weight trait: -1 to 1, 400 at 0)
      double w = weight <= 400 ? (weight - 400) / 300.0 * 0.4 : (weight - 400) / 500.0 * 0.62;
      CFNumberRef wn = CFNumberCreate(NULL, kCFNumberDoubleType, &w);
      const void* tk[] = {kCTFontWeightTrait};
      const void* tv[] = {wn};
      CFDictionaryRef traits = CFDictionaryCreate(NULL, tk, tv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      const void* ak[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
      const void* av[] = {name, traits};
      CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
      base = CTFontCreateWithFontDescriptor(d, 16.0, NULL);
      CFRelease(d);
      CFRelease(attrs);
      CFRelease(traits);
      CFRelease(wn);
      CFRelease(name);
    }
  }
  if (base) {
    UniChar u[2];
    CFIndex n = 1;
    if (c >= 0x10000) {
      u[0] = 0xD800 + ((c - 0x10000) >> 10);
      u[1] = 0xDC00 + ((c - 0x10000) & 0x3FF);
      n = 2;
    } else {
      u[0] = (UniChar)c;
    }
    CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
    CTFontRef f = CTFontCreateForString(base, s, CFRangeMake(0, n));
    if (f) {
      CFURLRef url = (CFURLRef)CTFontCopyAttribute(f, kCTFontURLAttribute);
      CFStringRef ps = CTFontCopyPostScriptName(f);
      char path[1024], psn[512];
      if (url && ps && CFURLGetFileSystemRepresentation(url, true, (UInt8*)path, sizeof path) &&
          CFStringGetCString(ps, psn, sizeof psn, kCFStringEncodingUTF8)) {
        snprintf(out, sizeof out, "%s\t%s", path, psn);
      }
      if (url) CFRelease(url);
      if (ps) CFRelease(ps);
      CFRelease(f);
    }
    CFRelease(s);
    CFRelease(base);
  }
#endif
  int32_t len = (int32_t)strlen(out);
  moonbit_bytes_t b = moonbit_make_bytes(len, 0);
  memcpy(b, out, len);
  return b;
}

#ifdef __APPLE__
typedef struct { float* v; int n, cap; } fv_path_buf;
static void fv_push(fv_path_buf* p, float x) {
  if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 256; p->v = realloc(p->v, p->cap * sizeof(float)); }
  p->v[p->n++] = x;
}
static void fv_path_el(void* info, const CGPathElement* e) {
  fv_path_buf* p = info;
  int k = e->type == kCGPathElementMoveToPoint ? 1 : e->type == kCGPathElementAddLineToPoint ? 1
        : e->type == kCGPathElementAddQuadCurveToPoint ? 2 : e->type == kCGPathElementAddCurveToPoint ? 3 : 0;
  fv_push(p, (float)e->type);
  for (int i = 0; i < k; i++) { fv_push(p, (float)e->points[i].x); fv_push(p, (float)e->points[i].y); }
}
#endif

// Glyph `glyph` of the face named `ps` (NUL-terminated PostScript name) at weight `wght` (0: the face's own), its em
// `upem` units: its outline as CoreText draws it, in font units, y up, as float32s: an element's kind (0 move, 1 line,
// 2 quad, 3 cubic, 4 close) then its points. Empty when there is no such face. (PingFang keeps its outlines only in
// Apple's `hvgl`, which CoreText reads.)
moonbit_bytes_t fv_ct_outline(moonbit_bytes_t ps, int32_t glyph, int32_t upem, int32_t wght) {
  int32_t len = 0;
  float* data = NULL;
#ifndef __APPLE__
  (void)ps;
  (void)glyph;
  (void)upem;
  (void)wght;
#else
  CFStringRef name = CFStringCreateWithCString(NULL, (const char*)ps, kCFStringEncodingUTF8);
  if (name) {
    CTFontRef f = CTFontCreateWithName(name, (CGFloat)upem, NULL);
    // (a name CoreText does not know gives another face: none then)
    if (f) {
      CFStringRef got = CTFontCopyPostScriptName(f);
      int same = got && CFStringCompare(got, name, 0) == kCFCompareEqualTo;
      if (got) CFRelease(got);
      if (!same) { CFRelease(f); f = NULL; }
    }
    if (f && wght > 0) {
      // 'wght' as the variation axis's tag
      int32_t tag = ('w' << 24) | ('g' << 16) | ('h' << 8) | 't';
      double w = wght;
      CFNumberRef tn = CFNumberCreate(NULL, kCFNumberSInt32Type, &tag);
      CFNumberRef wn = CFNumberCreate(NULL, kCFNumberDoubleType, &w);
      const void* k[] = {tn};
      const void* v[] = {wn};
      CFDictionaryRef var = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      const void* ak[] = {kCTFontVariationAttribute};
      const void* av[] = {var};
      CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes(attrs);
      CTFontRef g = CTFontCreateCopyWithAttributes(f, (CGFloat)upem, NULL, d);
      if (g) { CFRelease(f); f = g; }
      CFRelease(d); CFRelease(attrs); CFRelease(var); CFRelease(wn); CFRelease(tn);
    }
    if (f) {
      CGPathRef path = CTFontCreatePathForGlyph(f, (CGGlyph)glyph, NULL);
      if (path) {
        fv_path_buf p = {0};
        CGPathApply(path, &p, fv_path_el);
        data = p.v;
        len = p.n * 4;
        CGPathRelease(path);
      }
      CFRelease(f);
    }
    CFRelease(name);
  }
#endif
  moonbit_bytes_t b = moonbit_make_bytes(len, 0);
  if (len) memcpy(b, data, len);
  free(data);
  return b;
}
