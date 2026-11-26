// Platform bridge for `pion/packages/raster`:
// - macOS (__APPLE__): native CGBitmapContext hardware-accelerated 2D rasterizer (`FvCgSurface`),
//   CoreText text rendering, and ImageIO fallback for exotic formats.
// - Linux (!__APPLE__): zero C libraries; all 2D rasterization runs in pure Pion via `invar/skia` + `invar/pixels`.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

void pion_raster_malloc_trim(void) {
#ifdef __APPLE__
  malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
  (void)malloc_trim(0);
#endif
}

typedef struct {
  int32_t len;
  uint8_t data[];
} FvBuf;

static FvBuf *fv_buf_alloc(const void *src, int32_t len) {
  int32_t n = len < 0 ? 0 : len;
  FvBuf *b = (FvBuf *)malloc(sizeof(FvBuf) + (size_t)n + 1);
  b->len = n;
  if (n > 0 && src) memcpy(b->data, src, (size_t)n);
  b->data[n] = 0;
  return b;
}

int32_t pion_raster_buf_len(void *p) {
  if (!p) return 0;
  return ((FvBuf *)((uint8_t *)p - offsetof(FvBuf, data)))->len;
}

void pion_raster_buf_free(void *p) {
  if (p) free((uint8_t *)p - offsetof(FvBuf, data));
}

// ---------------------------------------------------------------------------
// Native macOS CGBitmapContext surface (`FvCgSurface`)
// ---------------------------------------------------------------------------

int32_t pion_raster_cg_available(void) {
#ifdef __APPLE__
  return 1;
#else
  return 0;
#endif
}

#ifdef __APPLE__
#define FV_MAX_AXIS_SEGS 64
#define FV_CG_GSTACK_MAX 16

typedef struct {
  double a, b, c, d, tx, ty;
  int32_t clip_ok;
  int32_t clip_int;
  double cl, ct, cr, cb;
} FvCgGState;

typedef struct {
  int32_t w;
  int32_t h;
  uint8_t *pixels;
  CGColorSpaceRef cs;
  CGContextRef ctx;
  CGMutablePathRef path;
  int32_t seg_ok;
  int32_t seg_n;
  int32_t sub_segs;
  int32_t has_cur;
  double cur_x;
  double cur_y;
  double segs[FV_MAX_AXIS_SEGS * 4];
  int32_t poly_ok;
  int32_t poly_n;
  int32_t poly_closed;
  double poly_pts[10];
  // Single full-circle fast path tracking (Chart.js scatter/line point markers)
  int32_t circ_ok;
  int32_t circ_has;
  int32_t circ_moves;
  double circ_cx;
  double circ_cy;
  double circ_r;
  // Exact CTM + axis-aligned clip rect stack for direct pixel fast paths
  FvCgGState gstack[FV_CG_GSTACK_MAX];
  int32_t gtop;
  int32_t goverflow;
} FvCgSurface;

static inline void fv_cg_reset_fast(FvCgSurface *s) {
  s->seg_ok = 1;
  s->seg_n = 0;
  s->sub_segs = 0;
  s->has_cur = 0;
  s->poly_ok = 1;
  s->poly_n = 0;
  s->poly_closed = 0;
  s->circ_ok = 1;
  s->circ_has = 0;
  s->circ_moves = 0;
}

static inline int fv_cg_is_int_coord(double v, int32_t *out) {
  double r = floor(v + 0.5);
  if (fabs(v - r) <= 1e-5 && r >= -1000000.0 && r <= 1000000.0) {
    *out = (int32_t)r;
    return 1;
  }
  return 0;
}

static inline int fv_cg_clip_bounds_for_box(
    const FvCgSurface *s, const FvCgGState *g,
    double bx0, double by0, double bx1, double by1,
    int32_t *cx0, int32_t *cy0, int32_t *cx1, int32_t *cy1) {
  if (!g->clip_ok) return 0;
  if (g->clip_int) {
    int32_t l = (int32_t)floor(g->cl + 0.5);
    int32_t t = (int32_t)floor(g->ct + 0.5);
    int32_t r = (int32_t)floor(g->cr + 0.5);
    int32_t b = (int32_t)floor(g->cb + 0.5);
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > s->w) r = s->w;
    if (b > s->h) b = s->h;
    *cx0 = l; *cy0 = t; *cx1 = r; *cy1 = b;
    return 1;
  }
  // Fractional axis-aligned clip: safe iff shape is strictly inside clip or strictly outside clip.
  if (bx1 <= g->cl || by1 <= g->ct || bx0 >= g->cr || by0 >= g->cb) {
    *cx0 = 0; *cy0 = 0; *cx1 = 0; *cy1 = 0;
    return 1;
  }
  if (bx0 >= g->cl && by0 >= g->ct && bx1 <= g->cr && by1 <= g->cb) {
    *cx0 = 0; *cy0 = 0; *cx1 = s->w; *cy1 = s->h;
    return 1;
  }
  return 0;
}

static inline void fv_cg_blend_pixel(uint8_t *p, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa) {
  if (sa == 0) return;
  if (sa == 255) {
    p[0] = (uint8_t)sr;
    p[1] = (uint8_t)sg;
    p[2] = (uint8_t)sb;
    p[3] = 255;
    return;
  }
  uint32_t pm_r = (sr * sa + 127) / 255;
  uint32_t pm_g = (sg * sa + 127) / 255;
  uint32_t pm_b = (sb * sa + 127) / 255;
  uint32_t inv = 255 - sa;
  p[0] = (uint8_t)(pm_r + (p[0] * inv + 127) / 255);
  p[1] = (uint8_t)(pm_g + (p[1] * inv + 127) / 255);
  p[2] = (uint8_t)(pm_b + (p[2] * inv + 127) / 255);
  p[3] = (uint8_t)(sa + (p[3] * inv + 127) / 255);
}

static int fv_cg_direct_clear_rect(FvCgSurface *s, double x, double y, double w, double h) {
  if (s->goverflow > 0) return 0;
  const FvCgGState *g = &s->gstack[s->gtop];
  if (!g->clip_ok || g->b != 0.0 || g->c != 0.0 || g->a <= 0.0 || g->d <= 0.0) return 0;
  int32_t x0, y0, x1, y1, cx0, cy0, cx1, cy1;
  double px0 = x * g->a + g->tx, py0 = y * g->d + g->ty;
  double px1 = (x + w) * g->a + g->tx, py1 = (y + h) * g->d + g->ty;
  if (!fv_cg_is_int_coord(px0, &x0) || !fv_cg_is_int_coord(py0, &y0) ||
      !fv_cg_is_int_coord(px1, &x1) || !fv_cg_is_int_coord(py1, &y1)) {
    return 0;
  }
  if (!fv_cg_clip_bounds_for_box(s, g, px0, py0, px1, py1, &cx0, &cy0, &cx1, &cy1)) return 0;
  if (x0 < cx0) x0 = cx0;
  if (y0 < cy0) y0 = cy0;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  if (x0 >= x1 || y0 >= y1) return 1;
  size_t row_bytes = (size_t)(x1 - x0) * 4;
  if (x0 == 0 && x1 == s->w) {
    memset(s->pixels + (size_t)y0 * (size_t)s->w * 4, 0, (size_t)(y1 - y0) * row_bytes);
  } else {
    for (int32_t py = y0; py < y1; py++) {
      memset(s->pixels + ((size_t)py * (size_t)s->w + (size_t)x0) * 4, 0, row_bytes);
    }
  }
  return 1;
}

static int fv_cg_direct_fill_rect(FvCgSurface *s, double x, double y, double w, double h, uint32_t rgba) {
  if (s->goverflow > 0) return 0;
  const FvCgGState *g = &s->gstack[s->gtop];
  if (!g->clip_ok || g->b != 0.0 || g->c != 0.0 || g->a <= 0.0 || g->d <= 0.0) return 0;
  int32_t x0, y0, x1, y1, cx0, cy0, cx1, cy1;
  double px0 = x * g->a + g->tx, py0 = y * g->d + g->ty;
  double px1 = (x + w) * g->a + g->tx, py1 = (y + h) * g->d + g->ty;
  if (!fv_cg_is_int_coord(px0, &x0) || !fv_cg_is_int_coord(py0, &y0) ||
      !fv_cg_is_int_coord(px1, &x1) || !fv_cg_is_int_coord(py1, &y1)) {
    return 0;
  }
  if (!fv_cg_clip_bounds_for_box(s, g, px0, py0, px1, py1, &cx0, &cy0, &cx1, &cy1)) return 0;
  if (x0 < cx0) x0 = cx0;
  if (y0 < cy0) y0 = cy0;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  if (x0 >= x1 || y0 >= y1) return 1;
  uint32_t sr = (rgba >> 24) & 0xFF;
  uint32_t sg = (rgba >> 16) & 0xFF;
  uint32_t sb = (rgba >> 8) & 0xFF;
  uint32_t sa = rgba & 0xFF;
  if (sa == 255) {
    uint8_t px_bytes[4] = {(uint8_t)sr, (uint8_t)sg, (uint8_t)sb, 255};
    uint32_t px32;
    memcpy(&px32, px_bytes, 4);
    for (int32_t py = y0; py < y1; py++) {
      uint32_t *row = (uint32_t *)(s->pixels + ((size_t)py * (size_t)s->w + (size_t)x0) * 4);
      int32_t cnt = x1 - x0;
      for (int32_t i = 0; i < cnt; i++) row[i] = px32;
    }
  } else {
    uint32_t pm_r = (sr * sa + 127) / 255;
    uint32_t pm_g = (sg * sa + 127) / 255;
    uint32_t pm_b = (sb * sa + 127) / 255;
    uint32_t inv = 255 - sa;
    for (int32_t py = y0; py < y1; py++) {
      uint8_t *row = s->pixels + ((size_t)py * (size_t)s->w + (size_t)x0) * 4;
      int32_t cnt = x1 - x0;
      for (int32_t i = 0; i < cnt; i++) {
        uint8_t *p = row + i * 4;
        p[0] = (uint8_t)(pm_r + (p[0] * inv + 127) / 255);
        p[1] = (uint8_t)(pm_g + (p[1] * inv + 127) / 255);
        p[2] = (uint8_t)(pm_b + (p[2] * inv + 127) / 255);
        p[3] = (uint8_t)(sa + (p[3] * inv + 127) / 255);
      }
    }
  }
  return 1;
}

static int fv_cg_direct_fill_circle(FvCgSurface *s, uint32_t rgba) {
  if (!s->circ_ok || !s->circ_has || s->goverflow > 0) return 0;
  const FvCgGState *g = &s->gstack[s->gtop];
  if (!g->clip_ok || g->b != 0.0 || g->c != 0.0 || g->a <= 0.0 || fabs(g->a - g->d) > 1e-6) return 0;
  double pr = s->circ_r * g->a;
  if (pr <= 0.0 || pr > 64.0) return 0;
  double pcx = s->circ_cx * g->a + g->tx;
  double pcy = s->circ_cy * g->d + g->ty;
  int32_t cx0, cy0, cx1, cy1;
  if (!fv_cg_clip_bounds_for_box(s, g, pcx - pr - 1.0, pcy - pr - 1.0, pcx + pr + 1.0, pcy + pr + 1.0, &cx0, &cy0, &cx1, &cy1)) {
    return 0;
  }
  int32_t x0 = (int32_t)floor(pcx - pr - 1.0);
  int32_t y0 = (int32_t)floor(pcy - pr - 1.0);
  int32_t x1 = (int32_t)ceil(pcx + pr + 1.0);
  int32_t y1 = (int32_t)ceil(pcy + pr + 1.0);
  if (x0 < cx0) x0 = cx0;
  if (y0 < cy0) y0 = cy0;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  if (x0 >= x1 || y0 >= y1) return 1;
  uint32_t sr = (rgba >> 24) & 0xFF;
  uint32_t sg = (rgba >> 16) & 0xFF;
  uint32_t sb = (rgba >> 8) & 0xFF;
  uint32_t sa = rgba & 0xFF;
  double r_outer = pr + 0.5;
  double r_inner = pr > 0.5 ? pr - 0.5 : 0.0;
  double r_outer2 = r_outer * r_outer;
  double r_inner2 = r_inner * r_inner;
  for (int32_t py = y0; py < y1; py++) {
    double dy = ((double)py + 0.5) - pcy;
    double dy2 = dy * dy;
    if (dy2 >= r_outer2) continue;
    uint8_t *row = s->pixels + (size_t)py * (size_t)s->w * 4;
    for (int32_t px = x0; px < x1; px++) {
      double dx = ((double)px + 0.5) - pcx;
      double d2 = dx * dx + dy2;
      if (d2 >= r_outer2) continue;
      uint32_t eff_a;
      if (d2 <= r_inner2) {
        eff_a = sa;
      } else {
        double cov = r_outer - sqrt(d2);
        if (cov <= 0.0) continue;
        if (cov > 1.0) cov = 1.0;
        eff_a = (uint32_t)((double)sa * cov + 0.5);
        if (eff_a == 0) continue;
      }
      fv_cg_blend_pixel(row + (size_t)px * 4, sr, sg, sb, eff_a);
    }
  }
  return 1;
}

static int fv_cg_direct_stroke_circle(FvCgSurface *s, uint32_t rgba, double lw) {
  if (!s->circ_ok || !s->circ_has || s->goverflow > 0) return 0;
  const FvCgGState *g = &s->gstack[s->gtop];
  if (!g->clip_ok || g->b != 0.0 || g->c != 0.0 || g->a <= 0.0 || fabs(g->a - g->d) > 1e-6) return 0;
  double pr = s->circ_r * g->a;
  double plw = lw * g->a;
  double r_out = pr + plw * 0.5;
  if (r_out <= 0.0 || r_out > 64.0) return 0;
  double r_in = pr - plw * 0.5;
  double pcx = s->circ_cx * g->a + g->tx;
  double pcy = s->circ_cy * g->d + g->ty;
  int32_t cx0, cy0, cx1, cy1;
  if (!fv_cg_clip_bounds_for_box(s, g, pcx - r_out - 1.0, pcy - r_out - 1.0, pcx + r_out + 1.0, pcy + r_out + 1.0, &cx0, &cy0, &cx1, &cy1)) {
    return 0;
  }
  int32_t x0 = (int32_t)floor(pcx - r_out - 1.0);
  int32_t y0 = (int32_t)floor(pcy - r_out - 1.0);
  int32_t x1 = (int32_t)ceil(pcx + r_out + 1.0);
  int32_t y1 = (int32_t)ceil(pcy + r_out + 1.0);
  if (x0 < cx0) x0 = cx0;
  if (y0 < cy0) y0 = cy0;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  if (x0 >= x1 || y0 >= y1) return 1;
  uint32_t sr = (rgba >> 24) & 0xFF;
  uint32_t sg = (rgba >> 16) & 0xFF;
  uint32_t sb = (rgba >> 8) & 0xFF;
  uint32_t sa = rgba & 0xFF;
  double ro_hi = r_out + 0.5;
  double ro_hi2 = ro_hi * ro_hi;
  double ri_lo = r_in > 0.5 ? r_in - 0.5 : 0.0;
  double ri_lo2 = ri_lo * ri_lo;
  for (int32_t py = y0; py < y1; py++) {
    double dy = ((double)py + 0.5) - pcy;
    double dy2 = dy * dy;
    if (dy2 >= ro_hi2) continue;
    uint8_t *row = s->pixels + (size_t)py * (size_t)s->w * 4;
    for (int32_t px = x0; px < x1; px++) {
      double dx = ((double)px + 0.5) - pcx;
      double d2 = dx * dx + dy2;
      if (d2 >= ro_hi2) continue;
      if (ri_lo > 0.0 && d2 <= ri_lo2) continue;
      double dist = sqrt(d2);
      double cov_out = ro_hi - dist;
      if (cov_out <= 0.0) continue;
      if (cov_out > 1.0) cov_out = 1.0;
      double cov = cov_out;
      if (r_in > 0.0) {
        double cov_in = (r_in + 0.5) - dist;
        if (cov_in > 0.0) {
          if (cov_in > 1.0) cov_in = 1.0;
          cov -= cov_in;
          if (cov <= 0.0) continue;
        }
      }
      uint32_t eff_a = (uint32_t)((double)sa * cov + 0.5);
      if (eff_a == 0) continue;
      fv_cg_blend_pixel(row + (size_t)px * 4, sr, sg, sb, eff_a);
    }
  }
  return 1;
}

static inline void fv_cg_note_move(FvCgSurface *s, double x, double y) {
  if (s->circ_has || s->circ_moves > 0) s->circ_ok = 0;
  s->circ_moves++;
  s->has_cur = 1;
  s->sub_segs = 0;
  s->cur_x = x;
  s->cur_y = y;
  if (s->poly_ok && s->poly_n == 0) {
    s->poly_pts[0] = x;
    s->poly_pts[1] = y;
    s->poly_n = 1;
  } else {
    s->poly_ok = 0;
  }
}

static inline void fv_cg_note_line(FvCgSurface *s, double x, double y) {
  s->circ_ok = 0;
  if (s->seg_ok && s->has_cur && s->sub_segs == 0 && (s->cur_x == x || s->cur_y == y) && s->seg_n < FV_MAX_AXIS_SEGS) {
    int32_t k = s->seg_n * 4;
    s->segs[k + 0] = s->cur_x;
    s->segs[k + 1] = s->cur_y;
    s->segs[k + 2] = x;
    s->segs[k + 3] = y;
    s->seg_n++;
    s->sub_segs = 1;
  } else {
    s->seg_ok = 0;
  }
  s->cur_x = x;
  s->cur_y = y;
  s->has_cur = 1;
  if (s->poly_ok && s->poly_n > 0 && s->poly_n < 5) {
    s->poly_pts[s->poly_n * 2 + 0] = x;
    s->poly_pts[s->poly_n * 2 + 1] = y;
    if (s->poly_n == 4 && x == s->poly_pts[0] && y == s->poly_pts[1]) {
      s->poly_closed = 1;
    }
    s->poly_n++;
  } else {
    s->poly_ok = 0;
  }
}

static int fv_cg_extract_axis_rect(const FvCgSurface *s, CGRect *out, int require_closed) {
  if (s->poly_ok && (!require_closed || s->poly_closed) &&
      (s->poly_n == 4 || (s->poly_n == 5 && s->poly_pts[8] == s->poly_pts[0] && s->poly_pts[9] == s->poly_pts[1]))) {
    double x0 = s->poly_pts[0], y0 = s->poly_pts[1];
    double x1 = s->poly_pts[2], y1 = s->poly_pts[3];
    double x2 = s->poly_pts[4], y2 = s->poly_pts[5];
    double x3 = s->poly_pts[6], y3 = s->poly_pts[7];
    int ok_h = (y0 == y1 && x1 == x2 && y2 == y3 && x3 == x0);
    int ok_v = (x0 == x1 && y1 == y2 && x2 == x3 && y3 == y0);
    if (ok_h || ok_v) {
      double minx = fmin(fmin(x0, x1), fmin(x2, x3));
      double maxx = fmax(fmax(x0, x1), fmax(x2, x3));
      double miny = fmin(fmin(y0, y1), fmin(y2, y3));
      double maxy = fmax(fmax(y0, y1), fmax(y2, y3));
      *out = CGRectMake((CGFloat)minx, (CGFloat)miny, (CGFloat)(maxx - minx), (CGFloat)(maxy - miny));
      return 1;
    }
  }
  return CGPathIsRect(s->path, out) ? 1 : 0;
}

static int fv_cg_segs_can_fast_stroke(const FvCgSurface *s, uint32_t rgba, double lw) {
  if (!s->seg_ok || s->seg_n <= 0) return 0;
  if ((rgba & 0xFF) == 255 || s->seg_n == 1) return 1;
  // Translucent multi-segment stroke: only fast-path when all segments are parallel on one axis and non-overlapping.
  int all_h = 1, all_v = 1;
  for (int32_t i = 0; i < s->seg_n; i++) {
    double x0 = s->segs[i * 4 + 0], y0 = s->segs[i * 4 + 1];
    double x1 = s->segs[i * 4 + 2], y1 = s->segs[i * 4 + 3];
    if (y0 != y1) all_h = 0;
    if (x0 != x1) all_v = 0;
  }
  if (!all_h && !all_v) return 0;
  for (int32_t i = 0; i < s->seg_n; i++) {
    for (int32_t j = i + 1; j < s->seg_n; j++) {
      if (all_h) {
        if (fabs(s->segs[i * 4 + 1] - s->segs[j * 4 + 1]) < lw) return 0;
      } else {
        if (fabs(s->segs[i * 4 + 0] - s->segs[j * 4 + 0]) < lw) return 0;
      }
    }
  }
  return 1;
}

static inline void cg_set_fill_rgba(CGContextRef ctx, uint32_t rgba) {
  CGContextSetRGBFillColor(
      ctx,
      (CGFloat)((rgba >> 24) & 0xFF) / 255.0,
      (CGFloat)((rgba >> 16) & 0xFF) / 255.0,
      (CGFloat)((rgba >> 8) & 0xFF) / 255.0,
      (CGFloat)(rgba & 0xFF) / 255.0);
}

static inline void cg_set_stroke_rgba(CGContextRef ctx, uint32_t rgba) {
  CGContextSetRGBStrokeColor(
      ctx,
      (CGFloat)((rgba >> 24) & 0xFF) / 255.0,
      (CGFloat)((rgba >> 16) & 0xFF) / 255.0,
      (CGFloat)((rgba >> 8) & 0xFF) / 255.0,
      (CGFloat)(rgba & 0xFF) / 255.0);
}

static inline void fv_cg_stroke_axis_segs(FvCgSurface *s, uint32_t rgba, double lw) {
  double half = lw * 0.5;
  int cg_color_set = 0;
  for (int32_t i = 0; i < s->seg_n; i++) {
    double x0 = s->segs[i * 4 + 0];
    double y0 = s->segs[i * 4 + 1];
    double x1 = s->segs[i * 4 + 2];
    double y1 = s->segs[i * 4 + 3];
    if (y0 == y1) {
      double rx = x0 < x1 ? x0 : x1;
      double len = fabs(x1 - x0);
      if (len > 0.0 && !fv_cg_direct_fill_rect(s, rx, y0 - half, len, lw, rgba)) {
        if (!cg_color_set) { cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1; }
        CGContextFillRect(s->ctx, CGRectMake((CGFloat)rx, (CGFloat)(y0 - half), (CGFloat)len, (CGFloat)lw));
      }
    } else if (x0 == x1) {
      double ry = y0 < y1 ? y0 : y1;
      double len = fabs(y1 - y0);
      if (len > 0.0 && !fv_cg_direct_fill_rect(s, x0 - half, ry, lw, len, rgba)) {
        if (!cg_color_set) { cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1; }
        CGContextFillRect(s->ctx, CGRectMake((CGFloat)(x0 - half), (CGFloat)ry, (CGFloat)lw, (CGFloat)len));
      }
    }
  }
}

static inline void fv_cg_stroke_rect_fast(FvCgSurface *s, CGRect r, uint32_t rgba, double lw) {
  double half = lw * 0.5;
  double x = (double)r.origin.x, y = (double)r.origin.y;
  double w = (double)r.size.width, h = (double)r.size.height;
  int cg_color_set = 0;
  if (!fv_cg_direct_fill_rect(s, x - half, y - half, w + lw, lw, rgba)) {
    cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1;
    CGContextFillRect(s->ctx, CGRectMake((CGFloat)(x - half), (CGFloat)(y - half), (CGFloat)(w + lw), (CGFloat)lw));
  }
  if (!fv_cg_direct_fill_rect(s, x - half, y + h - half, w + lw, lw, rgba)) {
    if (!cg_color_set) { cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1; }
    CGContextFillRect(s->ctx, CGRectMake((CGFloat)(x - half), (CGFloat)(y + h - half), (CGFloat)(w + lw), (CGFloat)lw));
  }
  if (h > lw) {
    if (!fv_cg_direct_fill_rect(s, x - half, y + half, lw, h - lw, rgba)) {
      if (!cg_color_set) { cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1; }
      CGContextFillRect(s->ctx, CGRectMake((CGFloat)(x - half), (CGFloat)(y + half), (CGFloat)lw, (CGFloat)(h - lw)));
    }
    if (!fv_cg_direct_fill_rect(s, x + w - half, y + half, lw, h - lw, rgba)) {
      if (!cg_color_set) { cg_set_fill_rgba(s->ctx, rgba); cg_color_set = 1; }
      CGContextFillRect(s->ctx, CGRectMake((CGFloat)(x + w - half), (CGFloat)(y + half), (CGFloat)lw, (CGFloat)(h - lw)));
    }
  }
}
#endif

void *pion_raster_cg_new(int32_t w, int32_t h) {
#ifdef __APPLE__
  if (w <= 0) w = 1;
  if (h <= 0) h = 1;
  FvCgSurface *s = (FvCgSurface *)calloc(1, sizeof(FvCgSurface));
  if (!s) return NULL;
  s->w = w;
  s->h = h;
  size_t nbytes = (size_t)w * (size_t)h * 4;
  s->pixels = (uint8_t *)calloc(nbytes, 1);
  if (!s->pixels) {
    free(s);
    return NULL;
  }
  s->cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  s->ctx = CGBitmapContextCreate(
      s->pixels, (size_t)w, (size_t)h, 8, (size_t)w * 4, s->cs,
      (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
  if (!s->ctx) {
    CGColorSpaceRelease(s->cs);
    free(s->pixels);
    free(s);
    return NULL;
  }
  CGContextTranslateCTM(s->ctx, 0.0, (CGFloat)h);
  CGContextScaleCTM(s->ctx, 1.0, -1.0);
  CGContextSetShouldAntialias(s->ctx, true);
  CGContextSetAllowsAntialiasing(s->ctx, true);
  s->path = CGPathCreateMutable();
  s->gtop = 0;
  s->goverflow = 0;
  s->gstack[0].a = 1.0; s->gstack[0].b = 0.0;
  s->gstack[0].c = 0.0; s->gstack[0].d = 1.0;
  s->gstack[0].tx = 0.0; s->gstack[0].ty = 0.0;
  s->gstack[0].clip_ok = 1;
  s->gstack[0].clip_int = 1;
  s->gstack[0].cl = 0.0; s->gstack[0].ct = 0.0;
  s->gstack[0].cr = (double)w; s->gstack[0].cb = (double)h;
  fv_cg_reset_fast(s);
  return s;
#else
  (void)w; (void)h;
  return NULL;
#endif
}

void pion_raster_cg_free(void *p) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  if (s->path) CGPathRelease(s->path);
  if (s->ctx) CGContextRelease(s->ctx);
  if (s->cs) CGColorSpaceRelease(s->cs);
  if (s->pixels) free(s->pixels);
  free(s);
#else
  (void)p;
#endif
}

void pion_raster_cg_clear_rect(void *p, double x, double y, double w, double h) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || w <= 0.0 || h <= 0.0) return;
  if (fv_cg_direct_clear_rect(s, x, y, w, h)) return;
  CGContextClearRect(s->ctx, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
#else
  (void)p; (void)x; (void)y; (void)w; (void)h;
#endif
}

void pion_raster_cg_fill_rect(void *p, double x, double y, double w, double h, uint32_t rgba) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || w <= 0.0 || h <= 0.0 || (rgba & 0xFF) == 0) return;
  if (fv_cg_direct_fill_rect(s, x, y, w, h, rgba)) return;
  cg_set_fill_rgba(s->ctx, rgba);
  CGContextFillRect(s->ctx, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
#else
  (void)p; (void)x; (void)y; (void)w; (void)h; (void)rgba;
#endif
}

void pion_raster_cg_stroke_rect(void *p, double x, double y, double w, double h, uint32_t rgba, double lw) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || lw <= 0.0 || (rgba & 0xFF) == 0) return;
  cg_set_stroke_rgba(s->ctx, rgba);
  CGContextSetLineWidth(s->ctx, (CGFloat)lw);
  CGContextStrokeRect(s->ctx, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
#else
  (void)p; (void)x; (void)y; (void)w; (void)h; (void)rgba; (void)lw;
#endif
}

void pion_raster_cg_save(void *p) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  CGContextSaveGState(s->ctx);
  if (s->goverflow == 0 && s->gtop + 1 < FV_CG_GSTACK_MAX) {
    s->gstack[s->gtop + 1] = s->gstack[s->gtop];
    s->gtop++;
  } else {
    s->goverflow++;
  }
#else
  (void)p;
#endif
}

void pion_raster_cg_restore(void *p) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  CGContextRestoreGState(s->ctx);
  if (s->goverflow > 0) {
    s->goverflow--;
  } else if (s->gtop > 0) {
    s->gtop--;
  }
#else
  (void)p;
#endif
}

void pion_raster_cg_clip_rect(void *p, double x, double y, double w, double h) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  CGContextClipToRect(s->ctx, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
  if (s->goverflow == 0) {
    FvCgGState *g = &s->gstack[s->gtop];
    if (g->clip_ok && g->b == 0.0 && g->c == 0.0 && g->a > 0.0 && g->d > 0.0) {
      double l = x * g->a + g->tx, t = y * g->d + g->ty;
      double r = (x + w) * g->a + g->tx, b = (y + h) * g->d + g->ty;
      if (l > g->cl) g->cl = l;
      if (t > g->ct) g->ct = t;
      if (r < g->cr) g->cr = r;
      if (b < g->cb) g->cb = b;
      int32_t tmp;
      if (!fv_cg_is_int_coord(g->cl, &tmp) || !fv_cg_is_int_coord(g->ct, &tmp) ||
          !fv_cg_is_int_coord(g->cr, &tmp) || !fv_cg_is_int_coord(g->cb, &tmp)) {
        g->clip_int = 0;
      }
    } else {
      g->clip_ok = 0;
    }
  }
#else
  (void)p; (void)x; (void)y; (void)w; (void)h;
#endif
}

void pion_raster_cg_transform(void *p, double a, double b, double c, double d, double tx, double ty) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  CGContextConcatCTM(s->ctx, CGAffineTransformMake((CGFloat)a, (CGFloat)b, (CGFloat)c, (CGFloat)d, (CGFloat)tx, (CGFloat)ty));
  if (s->goverflow == 0) {
    FvCgGState *g = &s->gstack[s->gtop];
    double na = g->a * a + g->c * b;
    double nb = g->b * a + g->d * b;
    double nc = g->a * c + g->c * d;
    double nd = g->b * c + g->d * d;
    double ntx = g->a * tx + g->c * ty + g->tx;
    double nty = g->b * tx + g->d * ty + g->ty;
    g->a = na; g->b = nb; g->c = nc; g->d = nd; g->tx = ntx; g->ty = nty;
  }
#else
  (void)p; (void)a; (void)b; (void)c; (void)d; (void)tx; (void)ty;
#endif
}

void pion_raster_cg_begin_path(void *p) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return;
  if (s->path) CGPathRelease(s->path);
  s->path = CGPathCreateMutable();
  fv_cg_reset_fast(s);
#else
  (void)p;
#endif
}

void pion_raster_cg_move_to(void *p, double x, double y) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (s && s->path) {
    CGPathMoveToPoint(s->path, NULL, (CGFloat)x, (CGFloat)y);
    fv_cg_note_move(s, x, y);
  }
#else
  (void)p; (void)x; (void)y;
#endif
}

void pion_raster_cg_line_to(void *p, double x, double y) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path) return;
  if (CGPathIsEmpty(s->path)) {
    CGPathMoveToPoint(s->path, NULL, (CGFloat)x, (CGFloat)y);
    fv_cg_note_move(s, x, y);
  } else {
    CGPathAddLineToPoint(s->path, NULL, (CGFloat)x, (CGFloat)y);
    fv_cg_note_line(s, x, y);
  }
#else
  (void)p; (void)x; (void)y;
#endif
}

void pion_raster_cg_quad_to(void *p, double cpx, double cpy, double x, double y) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path) return;
  s->seg_ok = 0; s->poly_ok = 0; s->circ_ok = 0;
  if (CGPathIsEmpty(s->path)) CGPathMoveToPoint(s->path, NULL, (CGFloat)cpx, (CGFloat)cpy);
  CGPathAddQuadCurveToPoint(s->path, NULL, (CGFloat)cpx, (CGFloat)cpy, (CGFloat)x, (CGFloat)y);
#else
  (void)p; (void)cpx; (void)cpy; (void)x; (void)y;
#endif
}

void pion_raster_cg_cubic_to(void *p, double c1x, double c1y, double c2x, double c2y, double x, double y) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path) return;
  s->seg_ok = 0; s->poly_ok = 0; s->circ_ok = 0;
  if (CGPathIsEmpty(s->path)) CGPathMoveToPoint(s->path, NULL, (CGFloat)c1x, (CGFloat)c1y);
  CGPathAddCurveToPoint(s->path, NULL, (CGFloat)c1x, (CGFloat)c1y, (CGFloat)c2x, (CGFloat)c2y, (CGFloat)x, (CGFloat)y);
#else
  (void)p; (void)c1x; (void)c1y; (void)c2x; (void)c2y; (void)x; (void)y;
#endif
}

void pion_raster_cg_arc(void *p, double cx, double cy, double r, double sa, double ea, int32_t ccw) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path || r <= 0.0) return;
  s->seg_ok = 0; s->poly_ok = 0;
  if (s->circ_ok && !s->circ_has && fabs(ea - sa) >= 6.283185307179586 - 1e-4 &&
      (s->circ_moves == 0 || (fabs(s->cur_x - (cx + r * cos(sa))) < 1e-3 && fabs(s->cur_y - (cy + r * sin(sa))) < 1e-3))) {
    s->circ_has = 1;
    s->circ_cx = cx;
    s->circ_cy = cy;
    s->circ_r = r;
  } else {
    s->circ_ok = 0;
  }
  // CoreGraphics clockwise parameter in flipped Y-down CTM matches Canvas ccw flag
  CGPathAddArc(s->path, NULL, (CGFloat)cx, (CGFloat)cy, (CGFloat)r, (CGFloat)sa, (CGFloat)ea, ccw != 0);
#else
  (void)p; (void)cx; (void)cy; (void)r; (void)sa; (void)ea; (void)ccw;
#endif
}

void pion_raster_cg_close_path(void *p) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (s && s->path && !CGPathIsEmpty(s->path)) {
    s->seg_ok = 0;
    s->poly_closed = 1;
    CGPathCloseSubpath(s->path);
  }
#else
  (void)p;
#endif
}

void pion_raster_cg_path_csv(void *p, const void *csv_ptr, int64_t len) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !csv_ptr || len <= 0) return;
  if (s->path) CGPathRelease(s->path);
  s->path = CGPathCreateMutable();
  fv_cg_reset_fast(s);
  const char *cur = (const char *)csv_ptr;
  const char *end = cur + len;
  while (cur < end) {
    char *next = NULL;
    int code = (int)strtol(cur, &next, 10);
    if (next == cur) break;
    cur = next;
    if (cur < end && *cur == ',') cur++;
    if (code == 0 || code == 1) {
      double x = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double y = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      if (code == 0 || CGPathIsEmpty(s->path)) {
        CGPathMoveToPoint(s->path, NULL, (CGFloat)x, (CGFloat)y);
        fv_cg_note_move(s, x, y);
      } else {
        CGPathAddLineToPoint(s->path, NULL, (CGFloat)x, (CGFloat)y);
        fv_cg_note_line(s, x, y);
      }
    } else if (code == 2) {
      s->seg_ok = 0; s->poly_ok = 0; s->circ_ok = 0;
      double x1 = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double y1 = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double x2 = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double y2 = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double x = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double y = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      if (CGPathIsEmpty(s->path)) CGPathMoveToPoint(s->path, NULL, (CGFloat)x1, (CGFloat)y1);
      CGPathAddCurveToPoint(s->path, NULL, (CGFloat)x1, (CGFloat)y1, (CGFloat)x2, (CGFloat)y2, (CGFloat)x, (CGFloat)y);
    } else if (code == 3) {
      s->seg_ok = 0; s->poly_ok = 0; s->circ_ok = 0;
      double cx = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double cy = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double x = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double y = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      if (CGPathIsEmpty(s->path)) CGPathMoveToPoint(s->path, NULL, (CGFloat)cx, (CGFloat)cy);
      CGPathAddQuadCurveToPoint(s->path, NULL, (CGFloat)cx, (CGFloat)cy, (CGFloat)x, (CGFloat)y);
    } else if (code == 4) {
      s->seg_ok = 0;
      s->poly_closed = 1;
      if (!CGPathIsEmpty(s->path)) CGPathCloseSubpath(s->path);
    } else if (code == 5) {
      s->seg_ok = 0; s->poly_ok = 0;
      double cx = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double cy = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double r = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double sa = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      double ea = strtod(cur, &next); cur = next; if (cur < end && *cur == ',') cur++;
      int ccw = (int)strtol(cur, &next, 10); cur = next; if (cur < end && *cur == ',') cur++;
      if (r > 0.0) {
        if (s->circ_ok && !s->circ_has && fabs(ea - sa) >= 6.283185307179586 - 1e-4 &&
            (s->circ_moves == 0 || (fabs(s->cur_x - (cx + r * cos(sa))) < 1e-3 && fabs(s->cur_y - (cy + r * sin(sa))) < 1e-3))) {
          s->circ_has = 1;
          s->circ_cx = cx;
          s->circ_cy = cy;
          s->circ_r = r;
        } else {
          s->circ_ok = 0;
        }
        CGPathAddArc(s->path, NULL, (CGFloat)cx, (CGFloat)cy, (CGFloat)r, (CGFloat)sa, (CGFloat)ea, ccw != 0);
      }
    } else {
      break;
    }
  }
#else
  (void)p; (void)csv_ptr; (void)len;
#endif
}

void pion_raster_cg_fill(void *p, uint32_t rgba, int32_t even_odd) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path || (rgba & 0xFF) == 0 || CGPathIsEmpty(s->path)) return;
  if (fv_cg_direct_fill_circle(s, rgba)) return;
  CGRect rect;
  if (fv_cg_extract_axis_rect(s, &rect, 0)) {
    if (fv_cg_direct_fill_rect(s, (double)rect.origin.x, (double)rect.origin.y, (double)rect.size.width, (double)rect.size.height, rgba)) {
      return;
    }
    cg_set_fill_rgba(s->ctx, rgba);
    CGContextFillRect(s->ctx, rect);
    return;
  }
  cg_set_fill_rgba(s->ctx, rgba);
  CGContextBeginPath(s->ctx);
  CGContextAddPath(s->ctx, s->path);
  if (even_odd) CGContextEOFillPath(s->ctx);
  else CGContextFillPath(s->ctx);
#else
  (void)p; (void)rgba; (void)even_odd;
#endif
}

void pion_raster_cg_stroke(void *p, uint32_t rgba, double lw) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path || (rgba & 0xFF) == 0 || lw <= 0.0 || CGPathIsEmpty(s->path)) return;
  if (fv_cg_direct_stroke_circle(s, rgba, lw)) return;
  if (fv_cg_segs_can_fast_stroke(s, rgba, lw)) {
    fv_cg_stroke_axis_segs(s, rgba, lw);
    return;
  }
  CGRect rect;
  if (fv_cg_extract_axis_rect(s, &rect, 1)) {
    if ((rgba & 0xFF) == 255 || ((double)rect.size.width > lw && (double)rect.size.height > lw)) {
      fv_cg_stroke_rect_fast(s, rect, rgba, lw);
      return;
    }
  }
  cg_set_stroke_rgba(s->ctx, rgba);
  CGContextSetLineWidth(s->ctx, (CGFloat)lw);
  CGContextSetLineCap(s->ctx, kCGLineCapButt);
  CGContextSetLineJoin(s->ctx, kCGLineJoinMiter);
  CGContextSetMiterLimit(s->ctx, 10.0);
  CGContextSetLineDash(s->ctx, 0.0, NULL, 0);
  CGContextBeginPath(s->ctx);
  CGContextAddPath(s->ctx, s->path);
  CGContextStrokePath(s->ctx);
#else
  (void)p; (void)rgba; (void)lw;
#endif
}

void pion_raster_cg_stroke_styled(
    void *p, uint32_t rgba, double lw, int32_t cap, int32_t join, double miter,
    const double *dash, int32_t dash_n, double dash_off) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path || (rgba & 0xFF) == 0 || lw <= 0.0 || CGPathIsEmpty(s->path)) return;
  if ((!dash || dash_n <= 0) && fv_cg_direct_stroke_circle(s, rgba, lw)) return;
  if (cap == 0 && (!dash || dash_n <= 0)) {
    if (fv_cg_segs_can_fast_stroke(s, rgba, lw)) {
      fv_cg_stroke_axis_segs(s, rgba, lw);
      return;
    }
    if (join == 0) {
      CGRect rect;
      if (fv_cg_extract_axis_rect(s, &rect, 1)) {
        if ((rgba & 0xFF) == 255 || ((double)rect.size.width > lw && (double)rect.size.height > lw)) {
          fv_cg_stroke_rect_fast(s, rect, rgba, lw);
          return;
        }
      }
    }
  }
  cg_set_stroke_rgba(s->ctx, rgba);
  CGContextSetLineWidth(s->ctx, (CGFloat)lw);
  CGLineCap c = cap == 1 ? kCGLineCapRound : (cap == 2 ? kCGLineCapSquare : kCGLineCapButt);
  CGLineJoin j = join == 1 ? kCGLineJoinRound : (join == 2 ? kCGLineJoinBevel : kCGLineJoinMiter);
  CGContextSetLineCap(s->ctx, c);
  CGContextSetLineJoin(s->ctx, j);
  CGContextSetMiterLimit(s->ctx, miter > 0.0 ? (CGFloat)miter : 10.0);
  if (dash && dash_n > 0) {
    CGFloat tmp[64];
    int32_t n = dash_n > 64 ? 64 : dash_n;
    for (int32_t i = 0; i < n; i++) tmp[i] = (CGFloat)dash[i];
    CGContextSetLineDash(s->ctx, (CGFloat)dash_off, tmp, (size_t)n);
  } else {
    CGContextSetLineDash(s->ctx, 0.0, NULL, 0);
  }
  CGContextBeginPath(s->ctx);
  CGContextAddPath(s->ctx, s->path);
  CGContextStrokePath(s->ctx);
#else
  (void)p; (void)rgba; (void)lw; (void)cap; (void)join; (void)miter;
  (void)dash; (void)dash_n; (void)dash_off;
#endif
}

void pion_raster_cg_clip(void *p, int32_t even_odd) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !s->path || CGPathIsEmpty(s->path)) return;
  CGContextBeginPath(s->ctx);
  CGContextAddPath(s->ctx, s->path);
  if (even_odd) CGContextEOClip(s->ctx);
  else CGContextClip(s->ctx);
  if (s->goverflow == 0) {
    FvCgGState *g = &s->gstack[s->gtop];
    CGRect rect;
    if (g->clip_ok && g->b == 0.0 && g->c == 0.0 && g->a > 0.0 && g->d > 0.0 &&
        fv_cg_extract_axis_rect(s, &rect, 0)) {
      double l = (double)rect.origin.x * g->a + g->tx;
      double t = (double)rect.origin.y * g->d + g->ty;
      double r = ((double)rect.origin.x + (double)rect.size.width) * g->a + g->tx;
      double b = ((double)rect.origin.y + (double)rect.size.height) * g->d + g->ty;
      if (l > g->cl) g->cl = l;
      if (t > g->ct) g->ct = t;
      if (r < g->cr) g->cr = r;
      if (b < g->cb) g->cb = b;
      int32_t tmp;
      if (!fv_cg_is_int_coord(g->cl, &tmp) || !fv_cg_is_int_coord(g->ct, &tmp) ||
          !fv_cg_is_int_coord(g->cr, &tmp) || !fv_cg_is_int_coord(g->cb, &tmp)) {
        g->clip_int = 0;
      }
    } else {
      g->clip_ok = 0;
    }
  }
#else
  (void)p; (void)even_odd;
#endif
}

void pion_raster_cg_fill_linear_gradient(
    void *p, double rx, double ry, double rw, double rh,
    double x0, double y0, double x1, double y1,
    const double *offs, const int32_t *cols, int32_t n) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || n < 2 || rw <= 0.0 || rh <= 0.0) return;
  if (n > 64) n = 64;
  CGFloat locs[64];
  CGFloat comps[64 * 4];
  for (int32_t i = 0; i < n; i++) {
    locs[i] = (CGFloat)offs[i];
    uint32_t c = (uint32_t)cols[i];
    comps[i * 4 + 0] = (CGFloat)((c >> 24) & 0xFF) / 255.0;
    comps[i * 4 + 1] = (CGFloat)((c >> 16) & 0xFF) / 255.0;
    comps[i * 4 + 2] = (CGFloat)((c >> 8) & 0xFF) / 255.0;
    comps[i * 4 + 3] = (CGFloat)(c & 0xFF) / 255.0;
  }
  CGGradientRef grad = CGGradientCreateWithColorComponents(s->cs, comps, locs, (size_t)n);
  if (!grad) return;
  CGContextSaveGState(s->ctx);
  CGContextClipToRect(s->ctx, CGRectMake((CGFloat)rx, (CGFloat)ry, (CGFloat)rw, (CGFloat)rh));
  CGContextDrawLinearGradient(
      s->ctx, grad, CGPointMake((CGFloat)x0, (CGFloat)y0), CGPointMake((CGFloat)x1, (CGFloat)y1),
      kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
  CGContextRestoreGState(s->ctx);
  CGGradientRelease(grad);
#else
  (void)p; (void)rx; (void)ry; (void)rw; (void)rh;
  (void)x0; (void)y0; (void)x1; (void)y1; (void)offs; (void)cols; (void)n;
#endif
}

void pion_raster_cg_fill_radial_gradient(
    void *p, double rx, double ry, double rw, double rh,
    double c0x, double c0y, double r0, double c1x, double c1y, double r1,
    const double *offs, const int32_t *cols, int32_t n) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || n < 2 || rw <= 0.0 || rh <= 0.0) return;
  if (n > 64) n = 64;
  CGFloat locs[64];
  CGFloat comps[64 * 4];
  for (int32_t i = 0; i < n; i++) {
    locs[i] = (CGFloat)offs[i];
    uint32_t c = (uint32_t)cols[i];
    comps[i * 4 + 0] = (CGFloat)((c >> 24) & 0xFF) / 255.0;
    comps[i * 4 + 1] = (CGFloat)((c >> 16) & 0xFF) / 255.0;
    comps[i * 4 + 2] = (CGFloat)((c >> 8) & 0xFF) / 255.0;
    comps[i * 4 + 3] = (CGFloat)(c & 0xFF) / 255.0;
  }
  CGGradientRef grad = CGGradientCreateWithColorComponents(s->cs, comps, locs, (size_t)n);
  if (!grad) return;
  CGContextSaveGState(s->ctx);
  CGContextClipToRect(s->ctx, CGRectMake((CGFloat)rx, (CGFloat)ry, (CGFloat)rw, (CGFloat)rh));
  CGContextDrawRadialGradient(
      s->ctx, grad, CGPointMake((CGFloat)c0x, (CGFloat)c0y), (CGFloat)r0,
      CGPointMake((CGFloat)c1x, (CGFloat)c1y), (CGFloat)r1,
      kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
  CGContextRestoreGState(s->ctx);
  CGGradientRelease(grad);
#else
  (void)p; (void)rx; (void)ry; (void)rw; (void)rh;
  (void)c0x; (void)c0y; (void)r0; (void)c1x; (void)c1y; (void)r1;
  (void)offs; (void)cols; (void)n;
#endif
}

#ifdef __APPLE__
typedef struct {
  char family[64];
  double size;
  CTFontRef font;
} FvCtFontEntry;

static FvCtFontEntry fv_ct_font_cache[8];
static int fv_ct_font_next = 0;

static CTFontRef fv_ct_font_get(const char *fam, double size) {
  if (!fam || fam[0] == 0) fam = "Helvetica";
  double sz = size > 0.0 ? size : 10.0;
  for (int i = 0; i < 8; i++) {
    if (fv_ct_font_cache[i].font && fv_ct_font_cache[i].size == sz &&
        strcmp(fv_ct_font_cache[i].family, fam) == 0) {
      return fv_ct_font_cache[i].font;
    }
  }
  CFStringRef fname = CFStringCreateWithCString(NULL, fam, kCFStringEncodingUTF8);
  CTFontRef font = fname ? CTFontCreateWithName(fname, (CGFloat)sz, NULL) : NULL;
  if (fname) CFRelease(fname);
  if (!font) return NULL;
  if (strlen(fam) < sizeof(fv_ct_font_cache[0].family)) {
    int slot = fv_ct_font_next;
    fv_ct_font_next = (fv_ct_font_next + 1) & 7;
    if (fv_ct_font_cache[slot].font) CFRelease(fv_ct_font_cache[slot].font);
    strcpy(fv_ct_font_cache[slot].family, fam);
    fv_ct_font_cache[slot].size = sz;
    fv_ct_font_cache[slot].font = font;
    return font;
  }
  return font;
}
#endif

void pion_raster_cg_fill_text(
    void *p, const void *family_cstr, double size, double x, double y, uint32_t rgba, const void *text_cstr) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !text_cstr || ((const char *)text_cstr)[0] == 0) return;
  const char *fam = (family_cstr && ((const char *)family_cstr)[0]) ? (const char *)family_cstr : "Helvetica";
  int cached = strlen(fam) < sizeof(fv_ct_font_cache[0].family);
  CTFontRef font = fv_ct_font_get(fam, size);
  if (!font) return;
  CFStringRef str = CFStringCreateWithCString(NULL, (const char *)text_cstr, kCFStringEncodingUTF8);
  if (str) {
    CGFloat comps[4] = {
        (CGFloat)((rgba >> 24) & 0xFF) / 255.0,
        (CGFloat)((rgba >> 16) & 0xFF) / 255.0,
        (CGFloat)((rgba >> 8) & 0xFF) / 255.0,
        (CGFloat)(rgba & 0xFF) / 255.0,
    };
    CGColorRef color = CGColorCreate(s->cs, comps);
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void *vals[] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(astr);
    CGContextSetTextMatrix(s->ctx, CGAffineTransformMakeScale(1.0, -1.0));
    CGContextSetTextPosition(s->ctx, (CGFloat)x, (CGFloat)y);
    CTLineDraw(line, s->ctx);
    CFRelease(line);
    CFRelease(astr);
    CFRelease(attrs);
    CGColorRelease(color);
    CFRelease(str);
  }
  if (!cached) CFRelease(font);
#else
  (void)p; (void)family_cstr; (void)size; (void)x; (void)y; (void)rgba; (void)text_cstr;
#endif
}

void pion_raster_cg_draw_image(
    void *p, const void *rgba_ptr, int32_t sw, int32_t sh, double dx, double dy, double dw, double dh) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !rgba_ptr || sw <= 0 || sh <= 0 || dw <= 0.0 || dh <= 0.0) return;
  const uint8_t *rgba = (const uint8_t *)rgba_ptr;
  size_t nbytes = (size_t)sw * (size_t)sh * 4;
  uint8_t *pm = (uint8_t *)malloc(nbytes);
  if (!pm) return;
  for (size_t i = 0; i < nbytes; i += 4) {
    uint32_t a = rgba[i + 3];
    if (a == 255) {
      pm[i] = rgba[i]; pm[i + 1] = rgba[i + 1]; pm[i + 2] = rgba[i + 2]; pm[i + 3] = 255;
    } else if (a > 0) {
      pm[i] = (uint8_t)((rgba[i] * a) / 255);
      pm[i + 1] = (uint8_t)((rgba[i + 1] * a) / 255);
      pm[i + 2] = (uint8_t)((rgba[i + 2] * a) / 255);
      pm[i + 3] = (uint8_t)a;
    } else {
      pm[i] = pm[i + 1] = pm[i + 2] = pm[i + 3] = 0;
    }
  }
  CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, pm, nbytes, NULL);
  CGImageRef img = CGImageCreate(
      (size_t)sw, (size_t)sh, 8, 32, (size_t)sw * 4, s->cs,
      (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big),
      prov, NULL, true, kCGRenderingIntentDefault);
  if (img) {
    CGContextSaveGState(s->ctx);
    CGContextTranslateCTM(s->ctx, (CGFloat)dx, (CGFloat)(dy + dh));
    CGContextScaleCTM(s->ctx, 1.0, -1.0);
    CGContextDrawImage(s->ctx, CGRectMake(0.0, 0.0, (CGFloat)dw, (CGFloat)dh), img);
    CGContextRestoreGState(s->ctx);
    CGImageRelease(img);
  }
  CGDataProviderRelease(prov);
  free(pm);
#else
  (void)p; (void)rgba_ptr; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
#endif
}

void pion_raster_cg_put_image_data(void *p, const void *rgba_ptr, int32_t sw, int32_t sh, int32_t dx, int32_t dy) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || !rgba_ptr || sw <= 0 || sh <= 0) return;
  const uint8_t *rgba = (const uint8_t *)rgba_ptr;
  for (int32_t y = 0; y < sh; y++) {
    int32_t py = dy + y;
    if (py < 0 || py >= s->h) continue;
    for (int32_t x = 0; x < sw; x++) {
      int32_t px = dx + x;
      if (px < 0 || px >= s->w) continue;
      size_t si = ((size_t)y * (size_t)sw + (size_t)x) * 4;
      size_t di = ((size_t)py * (size_t)s->w + (size_t)px) * 4;
      uint32_t a = rgba[si + 3];
      s->pixels[di + 0] = (uint8_t)((rgba[si + 0] * a) / 255);
      s->pixels[di + 1] = (uint8_t)((rgba[si + 1] * a) / 255);
      s->pixels[di + 2] = (uint8_t)((rgba[si + 2] * a) / 255);
      s->pixels[di + 3] = (uint8_t)a;
    }
  }
#else
  (void)p; (void)rgba_ptr; (void)sw; (void)sh; (void)dx; (void)dy;
#endif
}

void *pion_raster_cg_get_image_data(void *p, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s || sw <= 0 || sh <= 0) return fv_buf_alloc(NULL, 0)->data;
  size_t nbytes = (size_t)sw * (size_t)sh * 4;
  FvBuf *out = fv_buf_alloc(NULL, (int32_t)nbytes);
  memset(out->data, 0, nbytes);
  for (int32_t y = 0; y < sh; y++) {
    int32_t py = sy + y;
    if (py < 0 || py >= s->h) continue;
    for (int32_t x = 0; x < sw; x++) {
      int32_t px = sx + x;
      if (px < 0 || px >= s->w) continue;
      size_t si = ((size_t)py * (size_t)s->w + (size_t)px) * 4;
      size_t di = ((size_t)y * (size_t)sw + (size_t)x) * 4;
      uint32_t a = s->pixels[si + 3];
      if (a == 255) {
        out->data[di + 0] = s->pixels[si + 0];
        out->data[di + 1] = s->pixels[si + 1];
        out->data[di + 2] = s->pixels[si + 2];
        out->data[di + 3] = 255;
      } else if (a > 0) {
        uint32_t r = (s->pixels[si + 0] * 255) / a;
        uint32_t g = (s->pixels[si + 1] * 255) / a;
        uint32_t b = (s->pixels[si + 2] * 255) / a;
        out->data[di + 0] = (uint8_t)(r > 255 ? 255 : r);
        out->data[di + 1] = (uint8_t)(g > 255 ? 255 : g);
        out->data[di + 2] = (uint8_t)(b > 255 ? 255 : b);
        out->data[di + 3] = (uint8_t)a;
      }
    }
  }
  return out->data;
#else
  (void)p; (void)sx; (void)sy; (void)sw; (void)sh;
  return fv_buf_alloc(NULL, 0)->data;
#endif
}

static uint32_t fv_crc32_table[256];
static int fv_crc32_ready = 0;

static void fv_crc32_init(void) {
  if (fv_crc32_ready) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    fv_crc32_table[i] = c;
  }
  fv_crc32_ready = 1;
}

static uint32_t fv_crc32_update(uint32_t c, const uint8_t *buf, size_t len) {
#if defined(__ARM_FEATURE_CRC32)
  const uint8_t *p = buf;
  while (len >= 8) {
    uint64_t v;
    memcpy(&v, p, 8);
    c = __crc32d(c, v);
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    uint32_t v;
    memcpy(&v, p, 4);
    c = __crc32w(c, v);
    p += 4;
    len -= 4;
  }
  if (len >= 2) {
    uint16_t v;
    memcpy(&v, p, 2);
    c = __crc32h(c, v);
    p += 2;
    len -= 2;
  }
  if (len > 0) {
    c = __crc32b(c, *p);
  }
  return c;
#else
  for (size_t i = 0; i < len; i++) {
    c = fv_crc32_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
  }
  return c;
#endif
}

static uint32_t fv_unpremul_mul16[256];
static int fv_unpremul_ready = 0;

static void fv_unpremul_init(void) {
  if (fv_unpremul_ready) return;
  fv_unpremul_mul16[0] = 0;
  for (uint32_t a = 1; a < 256; a++) {
    fv_unpremul_mul16[a] = (255u * 65536u + a - 1u) / a;
  }
  fv_unpremul_ready = 1;
}

static inline void fv_put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

int32_t pion_raster_png_size(int32_t w, int32_t h) {
  if (w <= 0 || h <= 0) return 0;
  size_t row_raw = 1 + (size_t)w * 4;
  size_t raw_total = row_raw * (size_t)h;
  size_t nblocks = (raw_total + 65534) / 65535;
  if (nblocks == 0) nblocks = 1;
  size_t zlib_len = 2 + nblocks * 5 + raw_total + 4;
  return (int32_t)(8 + 25 + 12 + zlib_len + 12);
}

static int32_t fv_encode_stored_png_into(const uint8_t *pixels, int32_t w, int32_t h, int unpremul, uint8_t *dst, int32_t cap) {
  if (!pixels || !dst || w <= 0 || h <= 0) return 0;
  int32_t need = pion_raster_png_size(w, h);
  if (cap < need) return 0;
  fv_crc32_init();
  if (unpremul) fv_unpremul_init();
  size_t row_raw = 1 + (size_t)w * 4;
  size_t raw_total = row_raw * (size_t)h;
  size_t nblocks = (raw_total + 65534) / 65535;
  if (nblocks == 0) nblocks = 1;
  size_t zlib_len = 2 + nblocks * 5 + raw_total + 4;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  memcpy(dst, sig, 8);
  size_t pos = 8;

  // IHDR
  fv_put_be32(dst + pos, 13); pos += 4;
  uint8_t *ihdr_start = dst + pos;
  memcpy(dst + pos, "IHDR", 4); pos += 4;
  fv_put_be32(dst + pos, (uint32_t)w); pos += 4;
  fv_put_be32(dst + pos, (uint32_t)h); pos += 4;
  dst[pos++] = 8; // bit depth
  dst[pos++] = 6; // RGBA
  dst[pos++] = 0; // compression
  dst[pos++] = 0; // filter
  dst[pos++] = 0; // interlace
  uint32_t crc = fv_crc32_update(0xFFFFFFFFu, ihdr_start, 17) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;

  // IDAT
  fv_put_be32(dst + pos, (uint32_t)zlib_len); pos += 4;
  uint8_t *idat_start = dst + pos;
  memcpy(dst + pos, "IDAT", 4); pos += 4;
  dst[pos++] = 0x78;
  dst[pos++] = 0x01;

  uint32_t s1 = 1, s2 = 0;
  size_t raw_pos = 0;
  size_t block_rem = 0;
  int32_t y = 0;
  size_t row_off = 0;
  uint8_t stack_row[4096];
  uint8_t *row_buf = row_raw <= sizeof(stack_row) ? stack_row : (uint8_t *)malloc(row_raw);
  while (raw_pos < raw_total) {
    if (block_rem == 0) {
      size_t rem = raw_total - raw_pos;
      size_t blen = rem > 65535 ? 65535 : rem;
      dst[pos++] = (rem <= 65535) ? 1 : 0;
      dst[pos++] = (uint8_t)(blen & 0xFF);
      dst[pos++] = (uint8_t)((blen >> 8) & 0xFF);
      uint16_t nlen = (uint16_t)(~blen);
      dst[pos++] = (uint8_t)(nlen & 0xFF);
      dst[pos++] = (uint8_t)((nlen >> 8) & 0xFF);
      block_rem = blen;
    }
    if (row_off == 0) {
      row_buf[0] = 0;
      const uint8_t *src_row = pixels + (size_t)y * (size_t)w * 4;
      uint8_t *rb = row_buf + 1;
      if (!unpremul) {
        memcpy(rb, src_row, (size_t)w * 4);
      } else {
        for (int32_t x = 0; x < w; x++) {
          size_t k = (size_t)x * 4;
          uint32_t a = src_row[k + 3];
          if (a == 255) {
            memcpy(rb + k, src_row + k, 4);
          } else if (a == 0) {
            uint32_t zero = 0;
            memcpy(rb + k, &zero, 4);
          } else {
            uint32_t m = fv_unpremul_mul16[a];
            uint32_t r = (src_row[k + 0] * m) >> 16;
            uint32_t g = (src_row[k + 1] * m) >> 16;
            uint32_t b = (src_row[k + 2] * m) >> 16;
            rb[k + 0] = (uint8_t)(r > 255u ? 255u : r);
            rb[k + 1] = (uint8_t)(g > 255u ? 255u : g);
            rb[k + 2] = (uint8_t)(b > 255u ? 255u : b);
            rb[k + 3] = (uint8_t)a;
          }
        }
      }
    }
    size_t avail = row_raw - row_off;
    size_t take = avail < block_rem ? avail : block_rem;
    const uint8_t *src = row_buf + row_off;
    memcpy(dst + pos, src, take);
    // Adler32 over chunk (modulo every <= 5552 bytes, 16-byte unrolled)
    size_t i = 0;
    while (i < take) {
      size_t n = take - i;
      if (n > 5552) n = 5552;
      const uint8_t *p = src + i;
      size_t j = 0;
      while (j + 16 <= n) {
        s1 += p[j + 0];  s2 += s1;
        s1 += p[j + 1];  s2 += s1;
        s1 += p[j + 2];  s2 += s1;
        s1 += p[j + 3];  s2 += s1;
        s1 += p[j + 4];  s2 += s1;
        s1 += p[j + 5];  s2 += s1;
        s1 += p[j + 6];  s2 += s1;
        s1 += p[j + 7];  s2 += s1;
        s1 += p[j + 8];  s2 += s1;
        s1 += p[j + 9];  s2 += s1;
        s1 += p[j + 10]; s2 += s1;
        s1 += p[j + 11]; s2 += s1;
        s1 += p[j + 12]; s2 += s1;
        s1 += p[j + 13]; s2 += s1;
        s1 += p[j + 14]; s2 += s1;
        s1 += p[j + 15]; s2 += s1;
        j += 16;
      }
      while (j < n) {
        s1 += p[j++];
        s2 += s1;
      }
      s1 %= 65521u;
      s2 %= 65521u;
      i += n;
    }
    pos += take;
    raw_pos += take;
    block_rem -= take;
    row_off += take;
    if (row_off == row_raw) {
      row_off = 0;
      y++;
    }
  }
  if (row_buf != stack_row) free(row_buf);
  fv_put_be32(dst + pos, (s2 << 16) | s1); pos += 4;
  crc = fv_crc32_update(0xFFFFFFFFu, idat_start, 4 + zlib_len) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;

  // IEND
  fv_put_be32(dst + pos, 0); pos += 4;
  uint8_t *iend_start = dst + pos;
  memcpy(dst + pos, "IEND", 4); pos += 4;
  crc = fv_crc32_update(0xFFFFFFFFu, iend_start, 4) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;
  return (int32_t)pos;
}

int32_t pion_raster_cg_write_png(void *p, void *out_ptr, int32_t cap) {
#ifdef __APPLE__
  FvCgSurface *s = (FvCgSurface *)p;
  if (!s) return 0;
  return fv_encode_stored_png_into(s->pixels, s->w, s->h, 1, (uint8_t *)out_ptr, cap);
#else
  (void)p; (void)out_ptr; (void)cap;
  return 0;
#endif
}

int32_t pion_raster_rgba_write_png(const void *rgba_ptr, int32_t w, int32_t h, void *out_ptr, int32_t cap) {
  return fv_encode_stored_png_into((const uint8_t *)rgba_ptr, w, h, 0, (uint8_t *)out_ptr, cap);
}

// Fast lossless RFC 1950/1951 PNG encoder (PNG Filter 1 Sub + Fixed-Huffman RLE dist=1/4).
// Falls back cleanly to stored PNG if output would not shrink.
typedef struct {
  uint8_t *dst;
  size_t pos;
  size_t cap;
  uint64_t bits;
  int nbits;
  int overflow;
} FvBitW;

static inline void fvbw_flush(FvBitW *bw) {
  while (bw->nbits >= 8) {
    if (bw->pos >= bw->cap) { bw->overflow = 1; bw->nbits = 0; return; }
    bw->dst[bw->pos++] = (uint8_t)(bw->bits & 0xFFu);
    bw->bits >>= 8;
    bw->nbits -= 8;
  }
}

static inline void fvbw_put(FvBitW *bw, uint32_t code, int n) {
  bw->bits |= ((uint64_t)code) << bw->nbits;
  bw->nbits += n;
  if (bw->nbits >= 32) fvbw_flush(bw);
}

static uint16_t fv_fixed_lit_code[288];
static uint8_t fv_fixed_lit_len[288];
static uint8_t fv_fixed_dist_rev[32];
static int fv_fixed_init_done = 0;

static uint32_t fv_bitrev(uint32_t v, int bits) {
  uint32_t r = 0;
  for (int i = 0; i < bits; i++) { r = (r << 1) | (v & 1u); v >>= 1; }
  return r;
}

static void fv_fixed_huff_init(void) {
  if (fv_fixed_init_done) return;
  for (int i = 0; i <= 143; i++) {
    fv_fixed_lit_code[i] = (uint16_t)fv_bitrev((uint32_t)(0x30 + i), 8);
    fv_fixed_lit_len[i] = 8;
  }
  for (int i = 144; i <= 255; i++) {
    fv_fixed_lit_code[i] = (uint16_t)fv_bitrev((uint32_t)(0x190 + (i - 144)), 9);
    fv_fixed_lit_len[i] = 9;
  }
  for (int i = 256; i <= 279; i++) {
    fv_fixed_lit_code[i] = (uint16_t)fv_bitrev((uint32_t)(i - 256), 7);
    fv_fixed_lit_len[i] = 7;
  }
  for (int i = 280; i <= 287; i++) {
    fv_fixed_lit_code[i] = (uint16_t)fv_bitrev((uint32_t)(0xC0 + (i - 280)), 8);
    fv_fixed_lit_len[i] = 8;
  }
  for (int i = 0; i < 32; i++) {
    fv_fixed_dist_rev[i] = (uint8_t)fv_bitrev((uint32_t)i, 5);
  }
  fv_fixed_init_done = 1;
}

static inline void fvbw_emit_match(FvBitW *bw, int len, int dist_code) {
  static const uint16_t lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
  };
  static const uint8_t lextra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
  };
  int sym;
  if (len <= 10) sym = len - 3;
  else if (len == 258) sym = 28;
  else if (len < 19) sym = 8 + ((len - 11) >> 1);
  else if (len < 35) sym = 12 + ((len - 19) >> 2);
  else if (len < 67) sym = 16 + ((len - 35) >> 3);
  else if (len < 131) sym = 20 + ((len - 67) >> 4);
  else sym = 24 + ((len - 131) >> 5);
  int lit = 257 + sym;
  fvbw_put(bw, fv_fixed_lit_code[lit], fv_fixed_lit_len[lit]);
  int eb = lextra[sym];
  if (eb) fvbw_put(bw, (uint32_t)(len - lbase[sym]), eb);
  fvbw_put(bw, fv_fixed_dist_rev[dist_code], 5);
}

int32_t pion_raster_rgba_write_png_fast(const void *rgba_ptr, int32_t w, int32_t h, void *out_ptr, int32_t cap) {
  const uint8_t *pixels = (const uint8_t *)rgba_ptr;
  uint8_t *dst = (uint8_t *)out_ptr;
  if (!pixels || !dst || w <= 0 || h <= 0) return 0;
  int32_t stored_need = pion_raster_png_size(w, h);
  if (cap < stored_need) return 0;
  // For tiny images (< 4 KB raw), stored PNG is already tiny and fastest.
  size_t raw_px_bytes = (size_t)w * (size_t)h * 4;
  if (raw_px_bytes < 4096) {
    return fv_encode_stored_png_into(pixels, w, h, 0, dst, cap);
  }
  fv_crc32_init();
  fv_fixed_huff_init();

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  memcpy(dst, sig, 8);
  size_t pos = 8;

  // IHDR
  fv_put_be32(dst + pos, 13); pos += 4;
  uint8_t *ihdr_start = dst + pos;
  memcpy(dst + pos, "IHDR", 4); pos += 4;
  fv_put_be32(dst + pos, (uint32_t)w); pos += 4;
  fv_put_be32(dst + pos, (uint32_t)h); pos += 4;
  dst[pos++] = 8;
  dst[pos++] = 6;
  dst[pos++] = 0;
  dst[pos++] = 0;
  dst[pos++] = 0;
  uint32_t crc = fv_crc32_update(0xFFFFFFFFu, ihdr_start, 17) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;

  // IDAT header placeholder
  size_t idat_len_pos = pos;
  pos += 4;
  uint8_t *idat_start = dst + pos;
  memcpy(dst + pos, "IDAT", 4); pos += 4;
  size_t zlib_start = pos;
  dst[pos++] = 0x78;
  dst[pos++] = 0x01;

  // Reserve room for Adler32 (4) + CRC (4) + IEND (12) + fallback safety margin
  size_t max_deflate_end = (size_t)stored_need - 32;
  FvBitW bw = { dst, pos, max_deflate_end, 0, 0, 0 };
  // BFINAL=1, BTYPE=01 (Fixed Huffman) -> 3 bits: 0b011 = 3
  fvbw_put(&bw, 3u, 3);

  size_t row_raw = 1 + (size_t)w * 4;
  uint8_t stack_row[8192];
  uint8_t *row_buf = row_raw <= sizeof(stack_row) ? stack_row : (uint8_t *)malloc(row_raw);
  if (!row_buf) return fv_encode_stored_png_into(pixels, w, h, 0, dst, cap);

  uint32_t s1 = 1, s2 = 0;
  int have_prev = 0;
  uint8_t prev_b1 = 0;
  uint8_t prev_b4[4] = {0, 0, 0, 0};

  for (int32_t y = 0; y < h && !bw.overflow; y++) {
    const uint8_t *src = pixels + (size_t)y * (size_t)w * 4;
    // PNG Filter 1 (Sub, bpp=4): turns solid runs into 0x00 and smooth gradients into constant 4-byte deltas.
    row_buf[0] = 1;
    row_buf[1] = src[0];
    row_buf[2] = src[1];
    row_buf[3] = src[2];
    row_buf[4] = src[3];
    size_t row_bytes = (size_t)w * 4;
    uint8_t *rb = row_buf + 1;
    for (size_t k = 4; k < row_bytes; k++) {
      rb[k] = (uint8_t)(src[k] - src[k - 4]);
    }
    // Adler32 over filtered row
    size_t ai = 0;
    while (ai < row_raw) {
      size_t n = row_raw - ai;
      if (n > 5552) n = 5552;
      const uint8_t *p = row_buf + ai;
      size_t j = 0;
      while (j + 16 <= n) {
        s1 += p[j+0]; s2 += s1; s1 += p[j+1]; s2 += s1;
        s1 += p[j+2]; s2 += s1; s1 += p[j+3]; s2 += s1;
        s1 += p[j+4]; s2 += s1; s1 += p[j+5]; s2 += s1;
        s1 += p[j+6]; s2 += s1; s1 += p[j+7]; s2 += s1;
        s1 += p[j+8]; s2 += s1; s1 += p[j+9]; s2 += s1;
        s1 += p[j+10]; s2 += s1; s1 += p[j+11]; s2 += s1;
        s1 += p[j+12]; s2 += s1; s1 += p[j+13]; s2 += s1;
        s1 += p[j+14]; s2 += s1; s1 += p[j+15]; s2 += s1;
        j += 16;
      }
      while (j < n) { s1 += p[j++]; s2 += s1; }
      s1 %= 65521u;
      s2 %= 65521u;
      ai += n;
    }
    // Emit row with fast dist=1 and dist=4 RLE matching
    size_t i = 0;
    while (i < row_raw && !bw.overflow) {
      // Check dist=1 run (most common: zeros from Filter Sub)
      if (have_prev && row_buf[i] == prev_b1) {
        size_t lim = row_raw - i;
        if (lim > 258) lim = 258;
        size_t run = 1;
        while (run < lim && row_buf[i + run] == prev_b1) run++;
        if (run >= 3) {
          fvbw_emit_match(&bw, (int)run, 0); // dist=1 -> code 0
          i += run;
          prev_b4[0] = prev_b4[1] = prev_b4[2] = prev_b4[3] = prev_b1;
          have_prev = 4;
          continue;
        }
      }
      // Check dist=4 repeating 4-byte pattern (constant-delta gradients)
      if (have_prev >= 4 && i + 3 < row_raw &&
          row_buf[i] == prev_b4[0] && row_buf[i + 1] == prev_b4[1] &&
          row_buf[i + 2] == prev_b4[2] && row_buf[i + 3] == prev_b4[3]) {
        size_t lim = row_raw - i;
        if (lim > 258) lim = 258;
        size_t run = 4;
        while (run < lim && row_buf[i + run] == prev_b4[run & 3]) run++;
        fvbw_emit_match(&bw, (int)run, 3); // dist=4 -> code 3
        // Update sliding 4-byte history
        size_t end = i + run;
        prev_b4[0] = row_buf[end - 4];
        prev_b4[1] = row_buf[end - 3];
        prev_b4[2] = row_buf[end - 2];
        prev_b4[3] = row_buf[end - 1];
        prev_b1 = prev_b4[3];
        i = end;
        continue;
      }
      uint8_t b = row_buf[i++];
      fvbw_put(&bw, fv_fixed_lit_code[b], fv_fixed_lit_len[b]);
      prev_b4[0] = prev_b4[1];
      prev_b4[1] = prev_b4[2];
      prev_b4[2] = prev_b4[3];
      prev_b4[3] = b;
      prev_b1 = b;
      if (have_prev < 4) have_prev++;
    }
  }
  if (row_buf != stack_row) free(row_buf);

  // EOB (256)
  fvbw_put(&bw, fv_fixed_lit_code[256], fv_fixed_lit_len[256]);
  fvbw_flush(&bw);
  if (bw.nbits > 0) {
    if (bw.pos >= bw.cap) bw.overflow = 1;
    else bw.dst[bw.pos++] = (uint8_t)(bw.bits & 0xFFu);
  }
  if (bw.overflow) {
    return fv_encode_stored_png_into(pixels, w, h, 0, dst, cap);
  }
  pos = bw.pos;
  fv_put_be32(dst + pos, (s2 << 16) | s1); pos += 4;
  size_t zlib_len = pos - zlib_start;
  fv_put_be32(dst + idat_len_pos, (uint32_t)zlib_len);
  crc = fv_crc32_update(0xFFFFFFFFu, idat_start, 4 + zlib_len) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;

  // IEND
  fv_put_be32(dst + pos, 0); pos += 4;
  uint8_t *iend_start = dst + pos;
  memcpy(dst + pos, "IEND", 4); pos += 4;
  crc = fv_crc32_update(0xFFFFFFFFu, iend_start, 4) ^ 0xFFFFFFFFu;
  fv_put_be32(dst + pos, crc); pos += 4;
  return (int32_t)pos;
}

// Renders text on macOS via CoreText into a w*h premultiplied RGBA8 buffer.
// Returns 0-length FvBuf on non-Apple platforms.
void *pion_raster_render_text(
    int32_t w, int32_t h,
    double a, double b, double c, double d, double tx, double ty,
    const void *family_cstr, double size, double x, double y, uint32_t rgba,
    const void *text_cstr) {
#ifdef __APPLE__
  if (w <= 0 || h <= 0 || !text_cstr || ((const char *)text_cstr)[0] == 0) {
    return fv_buf_alloc(NULL, 0)->data;
  }
  size_t nbytes = (size_t)w * (size_t)h * 4;
  FvBuf *out = fv_buf_alloc(NULL, (int32_t)nbytes);
  memset(out->data, 0, nbytes);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      out->data, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
      (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
  if (!ctx) {
    CGColorSpaceRelease(cs);
    return out->data;
  }
  CGContextTranslateCTM(ctx, 0.0, (CGFloat)h);
  CGContextScaleCTM(ctx, 1.0, -1.0);
  CGContextConcatCTM(ctx, CGAffineTransformMake(a, b, c, d, tx, ty));

  CFStringRef fname = CFStringCreateWithCString(
      NULL, (family_cstr && ((const char *)family_cstr)[0]) ? (const char *)family_cstr : "Helvetica",
      kCFStringEncodingUTF8);
  CTFontRef font = CTFontCreateWithName(fname, (CGFloat)size, NULL);
  CFRelease(fname);
  if (font) {
    CFStringRef str = CFStringCreateWithCString(NULL, (const char *)text_cstr, kCFStringEncodingUTF8);
    if (str) {
      CGFloat comps[4] = {
          (CGFloat)((rgba >> 24) & 0xFF) / 255.0,
          (CGFloat)((rgba >> 16) & 0xFF) / 255.0,
          (CGFloat)((rgba >> 8) & 0xFF) / 255.0,
          (CGFloat)(rgba & 0xFF) / 255.0,
      };
      CGColorRef color = CGColorCreate(cs, comps);
      const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
      const void *vals[] = {font, color};
      CFDictionaryRef attrs = CFDictionaryCreate(
          NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
      CTLineRef line = CTLineCreateWithAttributedString(astr);
      CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));
      CGContextSetTextPosition(ctx, x, y);
      CTLineDraw(line, ctx);
      CFRelease(line);
      CFRelease(astr);
      CFRelease(attrs);
      CGColorRelease(color);
      CFRelease(str);
    }
    CFRelease(font);
  }
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return out->data;
#else
  (void)w; (void)h; (void)a; (void)b; (void)c; (void)d; (void)tx; (void)ty;
  (void)family_cstr; (void)size; (void)x; (void)y; (void)rgba; (void)text_cstr;
  return fv_buf_alloc(NULL, 0)->data;
#endif
}

// macOS ImageIO fallback for formats not decoded by pure-Pion `invar/pixels` (e.g. GIF/HEIC/progressive JPEG).
void *pion_raster_decode_image_fallback(const void *data, int32_t len, void *out_wh) {
  int32_t *wh = (int32_t *)out_wh;
  wh[0] = 0;
  wh[1] = 0;
#ifdef __APPLE__
  if (!data || len <= 0) return fv_buf_alloc(NULL, 0)->data;
  CFDataRef cf = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)data, len, kCFAllocatorNull);
  CGImageSourceRef src = CGImageSourceCreateWithData(cf, NULL);
  CFRelease(cf);
  if (!src) return fv_buf_alloc(NULL, 0)->data;
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!img) return fv_buf_alloc(NULL, 0)->data;
  int32_t w = (int32_t)CGImageGetWidth(img);
  int32_t h = (int32_t)CGImageGetHeight(img);
  if (w <= 0 || h <= 0) {
    CGImageRelease(img);
    return fv_buf_alloc(NULL, 0)->data;
  }
  size_t nbytes = (size_t)w * (size_t)h * 4;
  uint8_t *pm = (uint8_t *)calloc(nbytes, 1);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      pm, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
      (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
  CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);

  FvBuf *out = fv_buf_alloc(NULL, (int32_t)nbytes);
  for (size_t i = 0; i < nbytes; i += 4) {
    uint32_t a = pm[i + 3];
    if (a == 255) {
      out->data[i] = pm[i];
      out->data[i + 1] = pm[i + 1];
      out->data[i + 2] = pm[i + 2];
      out->data[i + 3] = 255;
    } else if (a > 0) {
      out->data[i] = (uint8_t)((pm[i] * 255) / a);
      out->data[i + 1] = (uint8_t)((pm[i + 1] * 255) / a);
      out->data[i + 2] = (uint8_t)((pm[i + 2] * 255) / a);
      out->data[i + 3] = (uint8_t)a;
    } else {
      out->data[i] = 0;
      out->data[i + 1] = 0;
      out->data[i + 2] = 0;
      out->data[i + 3] = 0;
    }
  }
  free(pm);
  wh[0] = w;
  wh[1] = h;
  return out->data;
#else
  (void)data; (void)len;
  return fv_buf_alloc(NULL, 0)->data;
#endif
}

int32_t pion_raster_image_size_fallback(const void *data, int32_t len, void *out_wh) {
  int32_t *wh = (int32_t *)out_wh;
  wh[0] = 0;
  wh[1] = 0;
#ifdef __APPLE__
  if (!data || len <= 0) return 0;
  CFDataRef cf = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)data, len, kCFAllocatorNull);
  CGImageSourceRef src = CGImageSourceCreateWithData(cf, NULL);
  CFRelease(cf);
  if (!src) return 0;
  CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!props) return 0;
  int32_t w = 0, h = 0;
  CFNumberRef wn = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelWidth);
  CFNumberRef hn = (CFNumberRef)CFDictionaryGetValue(props, kCGImagePropertyPixelHeight);
  if (wn) CFNumberGetValue(wn, kCFNumberSInt32Type, &w);
  if (hn) CFNumberGetValue(hn, kCFNumberSInt32Type, &h);
  CFRelease(props);
  if (w > 0 && h > 0) {
    wh[0] = w;
    wh[1] = h;
    return 1;
  }
  return 0;
#else
  (void)data; (void)len;
  return 0;
#endif
}
