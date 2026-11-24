#include "c_compat/moonbit.h"
#include <chrono>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>

extern "C" {
// Pion runtime string helpers (ABI fixed by SPEC.md: PionString { int64_t len; char data[]; }).
void *pion_str_from_cstr(const char *s, int64_t len);
int64_t pion_str_len(void *s);
void pion_rc_inc(void *obj);
void pion_rc_dec(void *obj);
void pion_rc_immortalize(void *obj);

// Symbols exported by native/js/v8_shim.cc
typedef moonbit_string_t (*fv_host_t)(int32_t, int32_t, int32_t, moonbit_string_t, moonbit_string_t);
typedef int32_t (*fv_host_int_t)(int32_t, int32_t, int32_t);

void fv_js_start_ref(fv_host_t fn);
void fv_js_set_int_host(fv_host_int_t fn);
moonbit_string_t fv_js_run(moonbit_string_t source);
moonbit_string_t fv_js_host_key(void);
void fv_js_set_origin(int32_t i, moonbit_string_t origin);
}

static moonbit_string_t utf8_to_mb(const char *s, size_t n) {
  if (n == 0) return moonbit_make_string(0, 0);
  // Fast path: pure ASCII (common for JS snippets, tag/attr names, CSS values, numbers)
  size_t i = 0;
  while (i < n && ((uint8_t)s[i] < 0x80)) i++;
  if (i == n) {
    moonbit_string_t m = moonbit_make_string((int32_t)n, 0);
    for (size_t k = 0; k < n; k++) m[k] = (uint16_t)(uint8_t)s[k];
    return m;
  }
  std::u16string u16;
  u16.reserve(n);
  for (size_t k = 0; k < i; k++) u16.push_back((char16_t)(uint8_t)s[k]);
  while (i < n) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp = 0;
    if (c < 0x80) {
      cp = c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)((uint8_t)s[i + 1] & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      cp = ((uint32_t)(c & 0x0F) << 12) |
           ((uint32_t)((uint8_t)s[i + 1] & 0x3F) << 6) |
           (uint32_t)((uint8_t)s[i + 2] & 0x3F);
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      cp = ((uint32_t)(c & 0x07) << 18) |
           ((uint32_t)((uint8_t)s[i + 1] & 0x3F) << 12) |
           ((uint32_t)((uint8_t)s[i + 2] & 0x3F) << 6) |
           (uint32_t)((uint8_t)s[i + 3] & 0x3F);
      i += 4;
    } else {
      cp = 0xFFFD;
      i += 1;
    }
    if (cp <= 0xFFFF) {
      u16.push_back((char16_t)cp);
    } else {
      cp -= 0x10000;
      u16.push_back((char16_t)(0xD800 + (cp >> 10)));
      u16.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
    }
  }
  moonbit_string_t m = moonbit_make_string((int32_t)u16.size(), 0);
  memcpy(m, u16.data(), u16.size() * sizeof(uint16_t));
  return m;
}

static std::string mb_to_utf8(moonbit_string_t m) {
  int32_t n = fv_mb_str_len(m);
  if (n <= 0) return std::string();
  std::string out;
  out.reserve((size_t)n);
  int32_t i = 0;
  while (i < n) {
    uint32_t cp = m[i++];
    if (cp < 0x80) {
      out.push_back((char)cp);
      continue;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF && i < n) {
      uint32_t lo = m[i];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
        i++;
      }
    }
    if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

static void *g_empty_pion_str = nullptr;

static inline void *empty_pion_str() {
  if (!g_empty_pion_str) {
    g_empty_pion_str = pion_str_from_cstr("", 0);
    pion_rc_immortalize(g_empty_pion_str);
  }
  return g_empty_pion_str;
}

// Convert MoonBit UTF-16 string directly to PionString with stack-buffer ASCII fast path.
static inline void *mb_to_pion_str(moonbit_string_t m) {
  int32_t n = fv_mb_str_len(m);
  if (n <= 0) return empty_pion_str();
  if (n <= 512) {
    char buf[512];
    int32_t i = 0;
    while (i < n && m[i] < 0x80) {
      buf[i] = (char)m[i];
      i++;
    }
    if (i == n) {
      return pion_str_from_cstr(buf, (int64_t)n);
    }
  }
  std::string u = mb_to_utf8(m);
  return pion_str_from_cstr(u.data(), (int64_t)u.size());
}

// Pion exported C callback signature:
//   extern "C" fn(op : Int, a : Int, b : Int, s : String, t : String) -> String
// Incoming String params are borrowed; returned String transfers ownership (RC=1) to C.
typedef void *(*pion_host_fn_t)(int32_t, int32_t, int32_t, void *, void *);

static pion_host_fn_t g_pion_host_fn = nullptr;

static moonbit_string_t pion_host_trampoline(
    int32_t op, int32_t a, int32_t b, moonbit_string_t s, moonbit_string_t t) {
  if (!g_pion_host_fn) return moonbit_make_string(0, 0);
  void *ps = mb_to_pion_str(s);
  void *pt = mb_to_pion_str(t);
  void *pr = g_pion_host_fn(op, a, b, ps, pt);
  moonbit_string_t out;
  if (pr) {
    int64_t rlen = pion_str_len(pr);
    const char *rdata = (const char *)pr + 8;
    out = utf8_to_mb(rdata, rlen > 0 ? (size_t)rlen : 0);
    pion_rc_dec(pr);
  } else {
    out = moonbit_make_string(0, 0);
  }
  pion_rc_dec(ps);
  pion_rc_dec(pt);
  return out;
}

typedef void (*fv_utf8_emit_t)(void *ctx, const char *data, int32_t len);
typedef void (*fv_host_utf8_t)(int32_t op, int32_t a, int32_t b,
    const char *s, int32_t slen, const char *t, int32_t tlen,
    fv_utf8_emit_t emit, void *ctx);

extern "C" void fv_js_set_utf8_host(fv_host_utf8_t fn);
extern "C" void fv_js_run_utf8(const char *src_utf8, int32_t src_len, fv_utf8_emit_t emit, void *ctx);

// Minimal ABI-compatible view of PionHeader / PionString (fixed by pion_rt.h)
// so large transient host_call strings (e.g. innerHTML batches > 992B, outside
// pion's 1024B slab) can reuse a thread-local PionString when Pion does not
// retain a reference.
struct PionBridgeHeader {
  uint32_t owner_tid;
  uint32_t local_rc;
  uint32_t shared_rc;
  uint16_t type_tag;
  uint16_t flags;
  void *drop_fn;
};
static_assert(sizeof(PionBridgeHeader) == 24, "PionHeader ABI mismatch");

struct PionBridgeString {
  int64_t len;
  char data[];
};

static constexpr uint16_t PION_BRIDGE_FLAG_NONREUSABLE = 1u | 2u | 4u; // FROZEN | IMMORTAL | SLAB

static const char kScratchZeroPad[262144] = {};

struct ScratchPionStr {
  void *ptr = nullptr;
  int32_t cap = 0;
  bool in_use = false;

  void *acquire(const char *s, int32_t slen, bool &used_scratch, bool allow_small = false) {
    if (in_use || slen > 262144 || (!allow_small && slen <= 992)) {
      used_scratch = false;
      return pion_str_from_cstr(s, (int64_t)slen);
    }
    if (!ptr || cap < slen) {
      if (ptr) {
        pion_rc_dec(ptr);
        ptr = nullptr;
        cap = 0;
      }
      int32_t want = slen <= 992 ? 1024 : 32768;
      while (want < slen && want < 262144) want <<= 1;
      if (want < slen) want = slen;
      // Allocate non-slab PionString of size `want` (> 992B) using BSS zero pad (zero heap temp alloc)
      ptr = pion_str_from_cstr(kScratchZeroPad, (int64_t)want);
      cap = want;
    }
    PionBridgeString *ps = reinterpret_cast<PionBridgeString *>(ptr);
    ps->len = (int64_t)slen;
    memcpy(ps->data, s, (size_t)slen);
    ps->data[slen] = '\0';
    in_use = true;
    used_scratch = true;
    return ptr;
  }

  void release(void *p, bool used_scratch) {
    if (!used_scratch) {
      pion_rc_dec(p);
      return;
    }
    in_use = false;
    PionBridgeHeader *hdr = reinterpret_cast<PionBridgeHeader *>(p) - 1;
    // Only keep in scratch cache if strictly unshared, RC == 1, and non-slab/non-frozen.
    if (hdr->owner_tid != 0 && hdr->local_rc == 1 && hdr->shared_rc == 0 &&
        (hdr->flags & PION_BRIDGE_FLAG_NONREUSABLE) == 0) {
      return;
    }
    // Pion retained or froze this string: detach cache and drop our own reference.
    ptr = nullptr;
    cap = 0;
    pion_rc_dec(p);
  }
};

static thread_local ScratchPionStr g_scratch_ps;
static thread_local ScratchPionStr g_scratch_pt;
static thread_local ScratchPionStr g_small_ps;
static thread_local ScratchPionStr g_small_pt;

static inline bool op_transient_strings(int32_t op) {
  switch (op) {
    case 2: case 9: case 11: case 13: case 17: case 18: case 20: case 21:
    case 24: case 32: case 41: case 42: case 45: case 47: case 66: case 69:
    case 70: case 77: case 79: case 81: case 90: case 92: case 103: case 110:
    case 115: case 116: case 125: case 126:
      return true;
    default:
      return false;
  }
}

static void pion_host_utf8_trampoline(
    int32_t op, int32_t a, int32_t b,
    const char *s, int32_t slen, const char *t, int32_t tlen,
    fv_utf8_emit_t emit, void *ctx) {
  if (!g_pion_host_fn) {
    emit(ctx, "", 0);
    return;
  }
  bool s_scratch = false, t_scratch = false;
  bool transient = op_transient_strings(op);
  bool s_small = transient && slen > 0 && slen <= 992;
  bool t_small = transient && tlen > 0 && tlen <= 992;
  void *ps = slen > 0
      ? (s_small ? g_small_ps.acquire(s, slen, s_scratch, true) : g_scratch_ps.acquire(s, slen, s_scratch))
      : empty_pion_str();
  void *pt = tlen > 0
      ? (t_small ? g_small_pt.acquire(t, tlen, t_scratch, true) : g_scratch_pt.acquire(t, tlen, t_scratch))
      : empty_pion_str();
  void *pr = g_pion_host_fn(op, a, b, ps, pt);
  if (pr) {
    int64_t rlen = pion_str_len(pr);
    const char *rdata = (const char *)pr + 8;
    emit(ctx, rdata, rlen > 0 ? (int32_t)rlen : 0);
    pion_rc_dec(pr);
  } else {
    emit(ctx, "", 0);
  }
  if (slen > 0) {
    if (s_small) g_small_ps.release(ps, s_scratch);
    else g_scratch_ps.release(ps, s_scratch);
  }
  if (tlen > 0) {
    if (t_small) g_small_pt.release(pt, t_scratch);
    else g_scratch_pt.release(pt, t_scratch);
  }
}

extern "C" {

void pion_js_start(void *cb_ptr) {
  g_pion_host_fn = reinterpret_cast<pion_host_fn_t>(cb_ptr);
  fv_js_set_utf8_host(pion_host_utf8_trampoline);
  fv_js_start_ref(pion_host_trampoline);
}

void pion_js_set_int_host(void *cb_ptr) {
  fv_js_set_int_host(reinterpret_cast<fv_host_int_t>(cb_ptr));
}

static thread_local std::string g_last_js_run;

static void js_run_emit_cb(void *, const char *data, int32_t len) {
  if (data && len > 0) {
    g_last_js_run.assign(data, (size_t)len);
  } else {
    g_last_js_run.clear();
  }
}

int32_t pion_js_run_len(const char *src_utf8, int32_t src_len) {
  g_last_js_run.clear();
  fv_js_run_utf8(src_utf8 ? src_utf8 : "", src_len > 0 ? src_len : 0, js_run_emit_cb, nullptr);
  return (int32_t)g_last_js_run.size();
}

void pion_js_run_copy(char *dst, int32_t n) {
  if (n > 0 && dst) memcpy(dst, g_last_js_run.data(), (size_t)n);
  g_last_js_run.clear();
}

void *pion_js_host_key(void) {
  moonbit_string_t mk = fv_js_host_key();
  std::string utf8 = mb_to_utf8(mk);
  moonbit_decref(mk);
  char *buf = (char *)malloc(utf8.size() + 1);
  if (utf8.size() > 0) memcpy(buf, utf8.data(), utf8.size());
  buf[utf8.size()] = 0;
  return (void *)buf;
}

void pion_js_set_origin(int32_t i, const char *origin_utf8, int32_t origin_len) {
  moonbit_string_t mo = utf8_to_mb(origin_utf8 ? origin_utf8 : "", origin_len > 0 ? (size_t)origin_len : 0);
  fv_js_set_origin(i, mo);
}

extern "C" void fv_js_set_script_ms(int32_t ms);

void pion_js_set_script_ms(int32_t ms) {
  std::string v = std::to_string(ms);
  setenv("FV_SCRIPT_MS", v.c_str(), 1);
  fv_js_set_script_ms(ms);
}

// Multi-threaded V8 Isolate Web Worker bridge (native/js/v8_worker.cc)
int32_t fv_worker_spawn(const char *script_utf8, int32_t script_len,
                        const char *url_utf8, int32_t url_len);
int32_t fv_worker_post(int32_t id, const char *data_utf8, int32_t data_len);
int32_t fv_worker_poll(int32_t id, char **out_utf8, int32_t *out_len);
void fv_worker_free(void *p);
void fv_worker_terminate(int32_t id);
void fv_worker_terminate_all(void);
int32_t fv_worker_active_count(void);
uint64_t fv_worker_thread_id(int32_t id);
uint64_t fv_worker_main_thread_id(void);

static thread_local std::string g_last_worker_msg;

int32_t pion_worker_spawn(const char *script_utf8, int32_t script_len,
                          const char *url_utf8, int32_t url_len) {
  return fv_worker_spawn(script_utf8, script_len, url_utf8, url_len);
}

int32_t pion_worker_post(int32_t id, const char *data_utf8, int32_t data_len) {
  return fv_worker_post(id, data_utf8, data_len);
}

int32_t pion_worker_poll_kind(int32_t id) {
  g_last_worker_msg.clear();
  char *buf = nullptr;
  int32_t len = 0;
  int32_t kind = fv_worker_poll(id, &buf, &len);
  if (buf) {
    if (len > 0) g_last_worker_msg.assign(buf, (size_t)len);
    fv_worker_free(buf);
  }
  return kind;
}

int32_t pion_worker_poll_len(void) {
  return (int32_t)g_last_worker_msg.size();
}

void pion_worker_poll_copy(char *dst, int32_t n) {
  if (n > 0 && dst) memcpy(dst, g_last_worker_msg.data(), (size_t)n);
  g_last_worker_msg.clear();
}

void pion_worker_terminate(int32_t id) { fv_worker_terminate(id); }
void pion_worker_terminate_all(void) { fv_worker_terminate_all(); }
int32_t pion_worker_active_count(void) { return fv_worker_active_count(); }
int64_t pion_worker_thread_id(int32_t id) { return (int64_t)fv_worker_thread_id(id); }
int64_t pion_worker_main_thread_id(void) { return (int64_t)fv_worker_main_thread_id(); }

void pion_worker_sleep_ms(int32_t ms) {
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Isolated Worlds (independent v8::Context per world_key) bridge (native/js/v8_shim.cc)
int32_t fv_js_world_create(int32_t ctx_id, const char *world_key, int32_t key_len);
void fv_js_world_eval_utf8(int32_t ctx_id, const char *world_key, int32_t key_len,
                           const char *src_utf8, int32_t src_len,
                           fv_utf8_emit_t emit, void *ctx);
int32_t fv_js_world_destroy(int32_t ctx_id, const char *world_key, int32_t key_len);
int32_t fv_js_world_count(int32_t ctx_id);

static thread_local std::string g_last_world_eval;

static void world_eval_emit_cb(void *, const char *data, int32_t len) {
  if (data && len > 0) {
    g_last_world_eval.assign(data, (size_t)len);
  } else {
    g_last_world_eval.clear();
  }
}

int32_t pion_js_world_create(int32_t ctx_id, const char *world_key, int32_t key_len) {
  return fv_js_world_create(ctx_id, world_key ? world_key : "", key_len > 0 ? key_len : 0);
}

int32_t pion_js_world_eval_len(int32_t ctx_id, const char *world_key, int32_t key_len,
                               const char *src_utf8, int32_t src_len) {
  g_last_world_eval.clear();
  fv_js_world_eval_utf8(ctx_id, world_key ? world_key : "", key_len > 0 ? key_len : 0,
                        src_utf8 ? src_utf8 : "", src_len > 0 ? src_len : 0,
                        world_eval_emit_cb, nullptr);
  return (int32_t)g_last_world_eval.size();
}

void pion_js_world_eval_copy(char *dst, int32_t n) {
  if (n > 0 && dst) memcpy(dst, g_last_world_eval.data(), (size_t)n);
  g_last_world_eval.clear();
}

int32_t pion_js_world_destroy(int32_t ctx_id, const char *world_key, int32_t key_len) {
  return fv_js_world_destroy(ctx_id, world_key ? world_key : "", key_len > 0 ? key_len : 0);
}

int32_t pion_js_world_count(int32_t ctx_id) {
  return fv_js_world_count(ctx_id);
}

}
