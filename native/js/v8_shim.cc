// V8 behind three calls: start, run a script, and a host function scripts call with (op, a, b, s, t).
#ifdef __APPLE__
// libc++, which V8's and this file's C++ need, said in this object (Mach-O's LC_LINKER_OPTION, as src/fetch/fetch.c
// says it for BoringSSL's): no program's moon.pkg names a C++ library, so the same link flags build on Linux, where
// the compiler tools/linux-cc/gcc puts in front of moon's adds libstdc++
__asm__(".linker_option \"-lc++\"\n");
#endif
#include <libplatform/libplatform.h>
// (moonbit.h declares memcpy itself unless it is a macro, without the noexcept glibc's <cstring> gives it in C++, which
// g++ refuses: the system's declaration taken, moonbit.h's skipped)
#include <cstring>
#define memcpy memcpy
#include <moonbit.h>
#include <v8.h>

#include <map>
#include <vector>
#include <set>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <cstring>
#include <zlib.h>
// BoringSSL's hashes (the curl-impersonate archive the host links; crypto.cc's, its headers not here): the same on
// every system
extern "C" {
uint8_t* SHA1(const uint8_t* data, size_t len, uint8_t* out);
uint8_t* SHA256(const uint8_t* data, size_t len, uint8_t* out);
uint8_t* SHA384(const uint8_t* data, size_t len, uint8_t* out);
uint8_t* SHA512(const uint8_t* data, size_t len, uint8_t* out);
}
#include <unordered_map>

using namespace v8;

extern "C" {
typedef moonbit_string_t (*fv_host_t)(int32_t, int32_t, int32_t, moonbit_string_t, moonbit_string_t);
// the host's calls that take and give numbers only (a node's parent, child, sibling, kind): no strings made either way
typedef int32_t (*fv_host_int_t)(int32_t, int32_t, int32_t);
}

// the Web Crypto primitives (crypto.cc)
void fv_crypto_install(Isolate* i, Local<ObjectTemplate> global);
#include "v8_worker.h"
#include <mutex>

static std::unique_ptr<Platform> default_platform;
static std::once_flag g_v8_platform_once;

extern "C" void* fv_v8_ensure_platform(void) {
  std::call_once(g_v8_platform_once, []() {
    // Single-tab renderer defaults: avoid excessive background worker contention across many cores,
    // keep shared UnboundScript bytecode (prelude.js) warm across iframe contexts, and reserve
    // TurboFan for genuinely hot loops (Maglev compiles at 400 invocations, avoiding speculative
    // TurboFan JSHeapBroker / AccountingAllocator zone churn on short-lived iframes).
    // FV_V8_FLAGS (applied after) can still override any flag.
    V8::SetFlagsFromString("--concurrent-marking-max-worker-num=1 --concurrent-turbofan-max-threads=1 --concurrent-maglev-max-threads=1 --no-flush-bytecode --invocation-count-for-turbofan=12000");
    if (const char* f = getenv("FV_V8_FLAGS")) V8::SetFlagsFromString(f);
    V8::InitializeICU();
    default_platform = platform::NewDefaultPlatform(2);
    V8::InitializePlatform(default_platform.get());
    V8::Initialize();
  });
  return default_platform.get();
}
static Isolate* isolate;
static Global<Context>* context = nullptr;
// every browsing context's global (the page's first, then its frames'), and which one runs scripts now
static std::vector<Global<Context>*> contexts;
extern "C" void fv_js_new_context(void);
static fv_host_t host_fn;
static fv_host_int_t host_int_fn;

typedef void (*fv_utf8_emit_t)(void* ctx, const char* data, int32_t len);
typedef void (*fv_host_utf8_t)(int32_t op, int32_t a, int32_t b,
    const char* s, int32_t slen, const char* t, int32_t tlen,
    fv_utf8_emit_t emit, void* ctx);
static fv_host_utf8_t host_utf8_fn = nullptr;

static int32_t g_dom_ver[3] = { 0, 0, 0 };
static constexpr int32_t FV_INT_BUF_CAP = 262144;
static int32_t g_int_buf[FV_INT_BUF_CAP];

// Isolate-scoped compiled UnboundScript cache (mirrors Chromium Blink V8CodeCache):
// compiles large classic scripts (prelude.js, editing.js, repeated iframe <script> bundles) once
// per Isolate and binds them to each new Context via UnboundScript::BindToCurrentContext().
struct CachedScriptEntry {
  uint64_t h1 = 0;
  uint64_t h2 = 0;
  size_t len = 0;
  std::string name;
  Global<UnboundScript> script;
};
static constexpr size_t FV_SCRIPT_CACHE_MAX = 64;
static std::vector<CachedScriptEntry> g_script_cache;

struct CachedModuleEntry {
  uint64_t h1 = 0;
  uint64_t h2 = 0;
  size_t len = 0;
  std::string url;
  std::vector<uint8_t> cached;
};
static constexpr size_t FV_MODULE_CACHE_MAX = 64;
static std::vector<CachedModuleEntry> g_module_cache;

static Global<ObjectTemplate> g_global_tmpl;
static Global<ObjectTemplate> g_world_global_tmpl;
static std::unordered_map<std::string, Global<Context>*> g_worlds;

static std::string world_map_key(int32_t ctx_id, const char* key, int32_t key_len) {
  std::string k = std::to_string(ctx_id);
  k.push_back('\x01');
  if (key && key_len > 0) k.append(key, (size_t)key_len);
  return k;
}

static int32_t current_ctx_id() {
  for (size_t i = 0; i < contexts.size(); i++) {
    if (contexts[i] == context) return (int32_t)i;
  }
  return 0;
}

static void destroy_worlds_for_ctx(int32_t ctx_id) {
  std::string prefix = std::to_string(ctx_id) + "\x01";
  bool disposed = false;
  for (auto it = g_worlds.begin(); it != g_worlds.end();) {
    if (ctx_id < 0 || it->first.compare(0, prefix.size(), prefix) == 0) {
      it->second->Reset();
      delete it->second;
      it = g_worlds.erase(it);
      disposed = true;
    } else {
      ++it;
    }
  }
  if (disposed && isolate) isolate->ContextDisposedNotification();
}

static inline uint64_t fv_fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

static inline void fv_hash_128(const char* p, size_t n, uint64_t& out1, uint64_t& out2) {
  uint64_t h1 = 0x9e3779b97f4a7c15ULL ^ (uint64_t)n;
  uint64_t h2 = 0xbf58476d1ce4e5b9ULL ^ ((uint64_t)n * 0x94d049bb133111ebULL);
  size_t i = 0;
  while (i + 8 <= n) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    h1 ^= w;
    h1 = (h1 << 27) | (h1 >> 37);
    h1 = h1 * 0x9e3779b185ebca87ULL + 0x52dce729ULL;
    h2 ^= w * 0x94d049bb133111ebULL;
    h2 = (h2 << 31) | (h2 >> 33);
    h2 = h2 * 0xc2b2ae3d27d4eb4fULL + 0x38495ab5ULL;
    i += 8;
  }
  uint64_t tail = 0;
  for (size_t j = i; j < n; j++) {
    tail = (tail << 8) | (uint8_t)p[j];
  }
  h1 ^= tail;
  h2 ^= tail * 0x9e3779b97f4a7c15ULL;
  out1 = fv_fmix64(h1 ^ (uint64_t)n);
  out2 = fv_fmix64(h2 ^ out1);
}

static const char* g_last_prelude_ptr = nullptr;
static size_t g_last_prelude_len = 0;
static size_t g_last_prelude_idx = 0;
static uint64_t g_last_prelude_w0 = 0;
static uint64_t g_last_prelude_wm = 0;
static uint64_t g_last_prelude_w1 = 0;

static inline void fv_sample_3words(const char* p, size_t n, uint64_t& w0, uint64_t& wm, uint64_t& w1) {
  memcpy(&w0, p, 8);
  memcpy(&wm, p + (n >> 1), 8);
  memcpy(&w1, p + n - 8, 8);
}

static Local<Script> fv_compile_cached_utf8(
    Isolate* iso, Local<Context> c, const char* src_utf8, size_t src_len,
    const std::string& origin_name, Local<String> origin_v8) {
  if (origin_name.empty() && src_len >= 100000 && src_utf8 == g_last_prelude_ptr && src_len == g_last_prelude_len &&
      g_last_prelude_idx < g_script_cache.size() && g_script_cache[g_last_prelude_idx].len == src_len &&
      g_script_cache[g_last_prelude_idx].name.empty()) {
    uint64_t w0, wm, w1;
    fv_sample_3words(src_utf8, src_len, w0, wm, w1);
    if (w0 == g_last_prelude_w0 && wm == g_last_prelude_wm && w1 == g_last_prelude_w1) {
      return g_script_cache[g_last_prelude_idx].script.Get(iso)->BindToCurrentContext();
    }
  }
  uint64_t h1 = 0, h2 = 0;
  fv_hash_128(src_utf8, src_len, h1, h2);
  for (size_t i = 0; i < g_script_cache.size(); i++) {
    auto& e = g_script_cache[i];
    if (e.h1 == h1 && e.h2 == h2 && e.len == src_len && e.name == origin_name) {
      if (origin_name.empty() && src_len >= 100000 && i < 4) {
        g_last_prelude_ptr = src_utf8;
        g_last_prelude_len = src_len;
        g_last_prelude_idx = i;
        fv_sample_3words(src_utf8, src_len, g_last_prelude_w0, g_last_prelude_wm, g_last_prelude_w1);
      }
      return e.script.Get(iso)->BindToCurrentContext();
    }
  }
  Local<String> v8_src;
  if (!String::NewFromUtf8(iso, src_utf8, NewStringType::kNormal, (int)src_len).ToLocal(&v8_src)) {
    return Local<Script>();
  }
  ScriptOrigin origin(origin_v8);
  ScriptCompiler::Source source(v8_src, origin);
  Local<UnboundScript> unbound;
  if (!ScriptCompiler::CompileUnboundScript(iso, &source).ToLocal(&unbound)) {
    return Local<Script>();
  }
  if (g_script_cache.size() >= FV_SCRIPT_CACHE_MAX) {
    // Keep first 4 entries (prelude.js / editing.js) and evict oldest page script.
    g_script_cache.erase(g_script_cache.begin() + 4);
  }
  CachedScriptEntry entry;
  entry.h1 = h1;
  entry.h2 = h2;
  entry.len = src_len;
  entry.name = origin_name;
  entry.script.Reset(iso, unbound);
  size_t idx = g_script_cache.size();
  g_script_cache.push_back(std::move(entry));
  if (origin_name.empty() && src_len >= 100000 && idx < 4) {
    g_last_prelude_ptr = src_utf8;
    g_last_prelude_len = src_len;
    g_last_prelude_idx = idx;
    fv_sample_3words(src_utf8, src_len, g_last_prelude_w0, g_last_prelude_wm, g_last_prelude_w1);
  }
  return unbound->BindToCurrentContext();
}

extern "C" void fv_js_set_int_host(fv_host_int_t fn) { host_int_fn = fn; }
extern "C" void fv_js_set_utf8_host(fv_host_utf8_t fn) { host_utf8_fn = fn; }
extern "C" void fv_js_set_dom_version(int32_t v) { g_dom_ver[0] = v; }
extern "C" void fv_js_set_doc_names_version(int32_t v) { g_dom_ver[1] = v; }
extern "C" void fv_js_set_detach_version(int32_t v) { g_dom_ver[2] = v; }
extern "C" int32_t fv_js_intbuf_cap(void) { return FV_INT_BUF_CAP; }
extern "C" int32_t* fv_js_intbuf_ptr(void) { return g_int_buf; }
extern "C" int32_t fv_js_intbuf_get(int32_t i) {
  return (i >= 0 && i < FV_INT_BUF_CAP) ? g_int_buf[i] : 0;
}
extern "C" void fv_js_intbuf_set(int32_t i, int32_t v) {
  if (i >= 0 && i < FV_INT_BUF_CAP) g_int_buf[i] = v;
}

static void install_dom_ver(Isolate* iso, Local<Context> c) {
  Context::Scope cs(c);
  auto store = ArrayBuffer::NewBackingStore(
      g_dom_ver, sizeof(g_dom_ver), [](void*, size_t, void*) {}, nullptr);
  Local<ArrayBuffer> ab = ArrayBuffer::New(iso, std::move(store));
  Local<Int32Array> view = Int32Array::New(ab, 0, 3);
  c->Global()->Set(c, String::NewFromUtf8Literal(iso, "__fvVer"), view).Check();

  auto ib_store = ArrayBuffer::NewBackingStore(
      g_int_buf, sizeof(g_int_buf), [](void*, size_t, void*) {}, nullptr);
  Local<ArrayBuffer> ib_ab = ArrayBuffer::New(iso, std::move(ib_store));
  Local<Int32Array> ib_view = Int32Array::New(ib_ab, 0, FV_INT_BUF_CAP);
  c->Global()->Set(c, String::NewFromUtf8Literal(iso, "__fvIntBuf"), ib_view).Check();
}

// The host function borrows its two strings (a MoonBit closure called through a FuncRef does not take them), so the
// caller lets go of them after; the answer is the caller's
static moonbit_string_t call_host(int32_t op, int32_t a, int32_t b, moonbit_string_t s, moonbit_string_t t) {
  moonbit_string_t r = host_fn(op, a, b, s, t);
  moonbit_decref(s);
  moonbit_decref(t);
  return r;
}

static moonbit_string_t to_mb(Isolate* i, Local<String> s) {
  int n = s->Length();
  moonbit_string_t m = moonbit_make_string(n, 0);
  s->WriteV2(i, 0, n, reinterpret_cast<uint16_t*>(m));
  return m;
}

static moonbit_string_t arg_str(const FunctionCallbackInfo<Value>& a, int i) {
  Isolate* iso = a.GetIsolate();
  if (i >= a.Length() || a[i]->IsUndefined()) return moonbit_make_string(0, 0);
  Local<String> s;
  if (!a[i]->ToString(iso->GetCurrentContext()).ToLocal(&s)) return moonbit_make_string(0, 0);
  return to_mb(iso, s);
}

static inline int32_t arg_int(const FunctionCallbackInfo<Value>& a, int i) {
  if (i >= a.Length()) return 0;
  Local<Value> v = a[i];
  if (v->IsInt32()) return v.As<Int32>()->Value();
  return v->Int32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(0);
}

static Eternal<String> g_str_nul;
static Eternal<String> g_str_zero;
static Eternal<String> g_str_one;

struct Utf8EmitCtx {
  Isolate* iso;
  Local<String> out;
};

static inline bool is_all_ascii(const uint8_t* p, int len) {
  int i = 0;
  uint64_t acc = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    acc |= w;
  }
  if (acc & 0x8080808080808080ULL) return false;
  uint8_t tail = 0;
  for (; i < len; i++) tail |= p[i];
  return (tail & 0x80) == 0;
}

static void emit_v8_utf8(void* p, const char* data, int32_t len) {
  Utf8EmitCtx* c = static_cast<Utf8EmitCtx*>(p);
  if (len <= 0) {
    c->out = String::Empty(c->iso);
  } else if (len == 1) {
    char ch = data[0];
    if (ch == '\0' && !g_str_nul.IsEmpty()) { c->out = g_str_nul.Get(c->iso); return; }
    if (ch == '0' && !g_str_zero.IsEmpty()) { c->out = g_str_zero.Get(c->iso); return; }
    if (ch == '1' && !g_str_one.IsEmpty()) { c->out = g_str_one.Get(c->iso); return; }
    if ((static_cast<uint8_t>(ch) & 0x80) == 0) {
      c->out = String::NewFromOneByte(c->iso, reinterpret_cast<const uint8_t*>(data), NewStringType::kNormal, 1).ToLocalChecked();
      return;
    }
    c->out = String::NewFromUtf8(c->iso, data, NewStringType::kNormal, 1).ToLocalChecked();
  } else {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(data);
    if (len <= 256 && is_all_ascii(u, len)) {
      c->out = String::NewFromOneByte(c->iso, u, NewStringType::kNormal, len).ToLocalChecked();
    } else {
      c->out = String::NewFromUtf8(c->iso, data, NewStringType::kNormal, len).ToLocalChecked();
    }
  }
}

static inline const char* arg_utf8(const FunctionCallbackInfo<Value>& a, int i,
    char* stack_buf, int stack_cap, std::string* tls_heap, std::unique_ptr<std::string>& local_heap, int32_t& out_len) {
  if (i >= a.Length()) { out_len = 0; return ""; }
  Local<Value> v = a[i];
  if (v->IsUndefined()) { out_len = 0; return ""; }
  Isolate* iso = a.GetIsolate();
  Local<String> s;
  if (v->IsString()) {
    s = v.As<String>();
  } else if (!v->ToString(iso->GetCurrentContext()).ToLocal(&s)) {
    out_len = 0; return "";
  }
  int len = s->Length();
  if (len == 0) { out_len = 0; return ""; }
  if (s->IsOneByte() && len <= stack_cap) {
    s->WriteOneByte(iso, reinterpret_cast<uint8_t*>(stack_buf), 0, len, String::NO_NULL_TERMINATION);
    if (is_all_ascii(reinterpret_cast<const uint8_t*>(stack_buf), len)) {
      out_len = len;
      return stack_buf;
    }
    if (len * 2 <= stack_cap) {
      int nbytes = s->WriteUtf8(iso, stack_buf, stack_cap, nullptr, String::NO_NULL_TERMINATION | String::REPLACE_INVALID_UTF8);
      out_len = nbytes;
      return stack_buf;
    }
  }
  int ulen = s->Utf8Length(iso);
  if (ulen <= stack_cap) {
    int nbytes = s->WriteUtf8(iso, stack_buf, stack_cap, nullptr, String::NO_NULL_TERMINATION | String::REPLACE_INVALID_UTF8);
    out_len = nbytes;
    return stack_buf;
  }
  std::string* hb = tls_heap;
  if (!hb) {
    local_heap = std::make_unique<std::string>();
    hb = local_heap.get();
  }
  hb->resize(ulen);
  int nbytes = s->WriteUtf8(iso, &(*hb)[0], ulen, nullptr, String::NO_NULL_TERMINATION | String::REPLACE_INVALID_UTF8);
  out_len = nbytes;
  return hb->data();
}

static int host_call_depth = 0;
static std::string host_call_sh;
static std::string host_call_th;

static void host_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  if (host_utf8_fn) {
    char sb[1024], tb[1024];
    std::unique_ptr<std::string> local_sh, local_th;
    bool top = (host_call_depth == 0);
    host_call_depth++;
    int32_t slen = 0, tlen = 0;
    const char* sp = arg_utf8(a, 3, sb, (int)sizeof(sb), top ? &host_call_sh : nullptr, local_sh, slen);
    const char* tp = arg_utf8(a, 4, tb, (int)sizeof(tb), top ? &host_call_th : nullptr, local_th, tlen);
    Utf8EmitCtx ctx{ iso, Local<String>() };
    host_utf8_fn(arg_int(a, 0), arg_int(a, 1), arg_int(a, 2), sp, slen, tp, tlen, emit_v8_utf8, &ctx);
    host_call_depth--;
    if (top) {
      if (host_call_sh.capacity() > 262144) { std::string().swap(host_call_sh); }
      if (host_call_th.capacity() > 262144) { std::string().swap(host_call_th); }
    }
    a.GetReturnValue().Set(ctx.out.IsEmpty() ? String::Empty(iso) : ctx.out);
    return;
  }
  moonbit_string_t r = call_host(arg_int(a, 0), arg_int(a, 1), arg_int(a, 2), arg_str(a, 3), arg_str(a, 4));
  int n = Moonbit_array_length(r);
  Local<String> s = String::NewFromTwoByte(iso, reinterpret_cast<const uint16_t*>(r), NewStringType::kNormal, n).ToLocalChecked();
  moonbit_decref(r);
  a.GetReturnValue().Set(s);
}

// __fvi(op, a, b): a call of the host's that answers a number
static void host_int_call(const FunctionCallbackInfo<Value>& a) {
  a.GetReturnValue().Set(host_int_fn(arg_int(a, 0), arg_int(a, 1), arg_int(a, 2)));
}

// ES modules: each fetched once by its URL; imports are resolved against the importing module's URL and fetched
// through the host (op 24: a=1 resolves a URL against a base, a=0 gets a URL's text).
static std::unordered_map<std::string, Global<Module>*> modules;
static std::unordered_map<int, std::string> module_urls;

static std::string utf8(Isolate* i, Local<Value> v) {
  String::Utf8Value u(i, v);
  return std::string(*u ? *u : "");
}

static Local<String> v8str(Isolate* i, const std::string& s) {
  return String::NewFromUtf8(i, s.c_str(), NewStringType::kNormal, (int)s.size()).ToLocalChecked();
}

static moonbit_string_t mbstr(const std::string& s) {
  // UTF-8 to the host's UTF-16, through V8
  Local<String> v = v8str(isolate, s);
  return to_mb(isolate, v);
}

static void emit_std_string(void* ctx, const char* s, int32_t len) {
  if (s && len > 0) static_cast<std::string*>(ctx)->assign(s, (size_t)len);
}

static std::string host_text(int op, int a, const std::string& s, const std::string& t) {
  if (host_utf8_fn) {
    std::string out;
    host_utf8_fn(op, a, 0, s.data(), (int32_t)s.size(), t.data(), (int32_t)t.size(), emit_std_string, &out);
    return out;
  }
  moonbit_string_t r = call_host(op, a, 0, mbstr(s), mbstr(t));
  int n = Moonbit_array_length(r);
  Local<String> v = String::NewFromTwoByte(isolate, reinterpret_cast<const uint16_t*>(r), NewStringType::kNormal, n).ToLocalChecked();
  moonbit_decref(r);
  return utf8(isolate, v);
}

// (a module is its global's: a frame's or a worker's of the same URL is one of its own)
static std::string module_key(Local<Context> c, const std::string& url) {
  for (size_t i = 0; i < contexts.size(); i++)
    if (!contexts[i]->IsEmpty() && contexts[i]->Get(isolate) == c) return std::to_string(i) + " " + url;
  return "? " + url;
}

static MaybeLocal<Module> load_module(Local<Context> c, const std::string& url, const std::string* code) {
  std::string key = module_key(c, url);
  auto it = modules.find(key);
  if (it != modules.end()) return it->second->Get(isolate);
  std::string text;
  if (code) {
    text = *code;
  } else if (url.rfind("blob:", 0) == 0) {
    // a blob: URL is the page's own (URL.createObjectURL): its text from the page
    Local<Value> f, r;
    Local<Value> argv[1] = {v8str(isolate, url)};
    if (c->Global()->Get(c, v8str(isolate, "__blobText")).ToLocal(&f) && f->IsFunction() &&
        f.As<Function>()->Call(c, c->Global(), 1, argv).ToLocal(&r) && r->IsString()) text = utf8(isolate, r);
  } else {
    text = host_text(24, 0, url, "");
  }
  ScriptOrigin origin(v8str(isolate, url), 0, 0, false, -1, Local<Value>(), false, false, true);
  Local<Module> m;
  if (text.empty()) {
    // nothing came for it (a failed fetch): said, and an error the import gets
    moonbit_decref(call_host(23, 2, 0, mbstr("module not loaded: " + url), mbstr("")));
    isolate->ThrowException(Exception::TypeError(v8str(isolate, "Failed to fetch module " + url)));
    return MaybeLocal<Module>();
  }
  Local<String> src_str;
  if (!String::NewFromUtf8(isolate, text.data(), NewStringType::kNormal, (int)text.size()).ToLocal(&src_str)) {
    return MaybeLocal<Module>();
  }
  if (text.size() >= 1024) {
    uint64_t h1 = 0, h2 = 0;
    fv_hash_128(text.data(), text.size(), h1, h2);
    for (size_t i = 0; i < g_module_cache.size(); i++) {
      auto& e = g_module_cache[i];
      if (e.h1 == h1 && e.h2 == h2 && e.len == text.size() && e.url == url) {
        auto* cd = new ScriptCompiler::CachedData(
            e.cached.data(), (int)e.cached.size(), ScriptCompiler::CachedData::BufferNotOwned);
        ScriptCompiler::Source src(src_str, origin, cd);
        if (!ScriptCompiler::CompileModule(isolate, &src, ScriptCompiler::kConsumeCodeCache).ToLocal(&m)) {
          return MaybeLocal<Module>();
        }
        modules[key] = new Global<Module>(isolate, m);
        module_urls[m->GetIdentityHash()] = url;
        return m;
      }
    }
    ScriptCompiler::Source src(src_str, origin);
    if (!ScriptCompiler::CompileModule(isolate, &src).ToLocal(&m)) return MaybeLocal<Module>();
    std::unique_ptr<ScriptCompiler::CachedData> cd(
        ScriptCompiler::CreateCodeCache(m->GetUnboundModuleScript()));
    if (cd && cd->data && cd->length > 0) {
      if (g_module_cache.size() >= FV_MODULE_CACHE_MAX) {
        g_module_cache.erase(g_module_cache.begin());
      }
      CachedModuleEntry entry;
      entry.h1 = h1;
      entry.h2 = h2;
      entry.len = text.size();
      entry.url = url;
      entry.cached.assign(cd->data, cd->data + cd->length);
      g_module_cache.push_back(std::move(entry));
    }
    modules[key] = new Global<Module>(isolate, m);
    module_urls[m->GetIdentityHash()] = url;
    return m;
  }
  ScriptCompiler::Source src(src_str, origin);
  if (!ScriptCompiler::CompileModule(isolate, &src).ToLocal(&m)) return MaybeLocal<Module>();
  modules[key] = new Global<Module>(isolate, m);
  module_urls[m->GetIdentityHash()] = url;
  return m;
}

static std::string page_url(Local<Context> c);

// The page's import map (<script type="importmap">): bare names and URL prefixes to URLs
static std::vector<std::pair<std::string, std::string>> import_map;

// A specifier against the URL it is imported from: what the import map says for it first (the name itself, or the
// longest prefix ending in /), an absolute one (blob:, data:, https:) as it is
static std::string resolve_url(const std::string& base, const std::string& spec) {
  const std::pair<std::string, std::string>* best = nullptr;
  for (auto& kv : import_map) {
    if (kv.first == spec) { best = &kv; break; }
    if (!kv.first.empty() && kv.first.back() == '/' && spec.rfind(kv.first, 0) == 0 &&
        (!best || kv.first.size() > best->first.size())) best = &kv;
  }
  if (best) return best->first == spec ? best->second : best->second + spec.substr(best->first.size());
  size_t colon = spec.find(':');
  if (colon != std::string::npos && colon > 1 && spec.find('/') > colon) return spec;
  return host_text(24, 1, base, spec);
}

// __fvImportMap(json): the page's import map, its addresses taken against the page's
static void import_map_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  Local<Context> c = iso->GetCurrentContext();
  Local<Value> v, imports;
  if (!a[0]->IsString() || !JSON::Parse(c, a[0].As<String>()).ToLocal(&v) || !v->IsObject()) return;
  if (!v.As<Object>()->Get(c, v8str(iso, "imports")).ToLocal(&imports) || !imports->IsObject()) return;
  std::string page = page_url(c);
  Local<Array> keys = imports.As<Object>()->GetOwnPropertyNames(c).ToLocalChecked();
  for (uint32_t i = 0; i < keys->Length(); i++) {
    Local<Value> k = keys->Get(c, i).ToLocalChecked(), target;
    if (!imports.As<Object>()->Get(c, k).ToLocal(&target) || !target->IsString()) continue;
    import_map.push_back({utf8(iso, k), host_text(24, 1, page, utf8(iso, target))});
  }
}

static MaybeLocal<Module> resolve_module(Local<Context> c, Local<String> spec, Local<FixedArray>, Local<Module> referrer) {
  std::string base = module_urls[referrer->GetIdentityHash()];
  return load_module(c, resolve_url(base, utf8(isolate, spec)), nullptr);
}

// The modules `root` imports, all the way down, fetched a level at a time with each level's requests in parallel
// (as a browser fetches a module graph): one at a time, NYTimes's page waited 26 s on its chunks in turn. Each is
// compiled here; one that fails is left for linking to load again and report.
static void load_graph(Local<Context> c, Local<Module> root) {
  if (root->GetStatus() != Module::kUninstantiated) return;
  std::vector<Global<Module>> level;
  level.emplace_back(isolate, root);
  while (!level.empty()) {
    HandleScope hs(isolate);
    std::vector<std::string> want;
    for (auto& g : level) {
      Local<Module> m = g.Get(isolate);
      std::string base = module_urls[m->GetIdentityHash()];
      Local<FixedArray> reqs = m->GetModuleRequests();
      for (int i = 0; i < reqs->Length(); i++) {
        Local<ModuleRequest> r = reqs->Get(c, i).As<ModuleRequest>();
        std::string url = resolve_url(base, utf8(isolate, r->GetSpecifier()));
        if (modules.count(module_key(c, url)) || url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0) continue;
        bool dup = false;
        for (auto& w : want) dup = dup || w == url;
        if (!dup) want.push_back(url);
      }
    }
    if (want.size() > 1) {
      std::string list;
      for (auto& w : want) list += (list.empty() ? "" : "\n") + w;
      host_text(24, 2, list, "");
    }
    std::vector<Global<Module>> next;
    for (auto& w : want) {
      TryCatch tc(isolate);
      Local<Module> m;
      if (load_module(c, w, nullptr).ToLocal(&m)) next.emplace_back(isolate, m);
    }
    level = std::move(next);
  }
}

// A module made ready and run: its evaluation's promise (rejected when it threw), or nothing when it could not be
// linked (the exception is pending).
static MaybeLocal<Value> run_module(Local<Context> c, Local<Module> m) {
  load_graph(c, m);
  if (m->GetStatus() == Module::kUninstantiated && !m->InstantiateModule(c, resolve_module).FromMaybe(false)) return MaybeLocal<Value>();
  Module::Status st = m->GetStatus();
  if (st == Module::kErrored) {
    Local<Promise::Resolver> r;
    if (!Promise::Resolver::New(c).ToLocal(&r)) return MaybeLocal<Value>();
    r->Reject(c, m->GetException()).Check();
    return r->GetPromise();
  }
  if (st != Module::kInstantiated && st != Module::kEvaluated) return MaybeLocal<Value>();
  return m->Evaluate(c);
}

static std::string page_url(Local<Context> c) {
  Local<Value> loc, href;
  if (c->Global()->Get(c, v8str(isolate, "location")).ToLocal(&loc) && loc->IsObject() &&
      loc.As<Object>()->Get(c, v8str(isolate, "href")).ToLocal(&href)) return utf8(isolate, href);
  return "";
}

// __fvModule(code or null, url): the page's module script, run (its own code, or fetched from url)
static void module_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  Local<Context> c = iso->GetCurrentContext();
  std::string url = utf8(iso, a[1]);
  std::string code;
  bool inline_ = a[0]->IsString();
  if (inline_) code = utf8(iso, a[0]);
  Local<Module> m;
  if (!load_module(c, url, inline_ ? &code : nullptr).ToLocal(&m)) return;
  Local<Value> p;
  if (!run_module(c, m).ToLocal(&p)) return;
  // a module that threw: its error, as a script's would be
  if (p->IsPromise() && p.As<Promise>()->State() == Promise::kRejected) {
    iso->ThrowException(p.As<Promise>()->Result());
    return;
  }
  a.GetReturnValue().Set(p);
}

// import()'s promise settled when the module's evaluation is (a module with top-level await, or one waiting on such a
// module, finishes later): [resolver, namespace] in the callback's data
static void dynamic_done(const FunctionCallbackInfo<Value>& a) {
  Local<Context> c = isolate->GetCurrentContext();
  Local<Array> d = a.Data().As<Array>();
  d->Get(c, 0).ToLocalChecked().As<Promise::Resolver>()->Resolve(c, d->Get(c, 1).ToLocalChecked()).Check();
}

static void dynamic_failed(const FunctionCallbackInfo<Value>& a) {
  Local<Context> c = isolate->GetCurrentContext();
  Local<Array> d = a.Data().As<Array>();
  d->Get(c, 0).ToLocalChecked().As<Promise::Resolver>()->Reject(c, a[0]).Check();
}

// import(): the module's graph fetched meanwhile (the page goes on, as a browser's does: a chunk got while the page's
// task waited held it for as long as the network took, 12 s for one of GitHub's), then run, its promise settled with
// it. Each waits in `pending_imports` until the files of its graph have all come (__fvPump, at each of its global's
// ticks, looks again).
struct PendingImport {
  Global<Context> c;
  Global<Promise::Resolver> r;
  std::string url;
};
static std::vector<PendingImport*> pending_imports;

// whether the host has these URLs' texts at hand (op 24 a=5, which asks for those it does not have yet)
static bool have_texts(const std::string& url) {
  if (url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0) return true;
  return host_text(24, 5, url, "") == "1";
}

// the URLs of a module graph not come yet, as far as what has come shows it (each compiled as it is reached)
static void graph_missing(Local<Context> c, const std::string& url, std::set<std::string>& seen, std::vector<std::string>& missing) {
  if (!seen.insert(url).second) return;
  Local<Module> m;
  auto it = modules.find(module_key(c, url));
  if (it != modules.end()) {
    m = it->second->Get(isolate);
  } else {
    if (!have_texts(url)) {
      missing.push_back(url);
      return;
    }
    // (one that does not compile is left for linking, which reports it)
    TryCatch tc(isolate);
    if (!load_module(c, url, nullptr).ToLocal(&m)) return;
  }
  Module::Status st = m->GetStatus();
  if (st == Module::kEvaluating || st == Module::kInstantiating) {
    missing.push_back(url);
    return;
  }
  if (st != Module::kUninstantiated) return;
  std::string base = module_urls[m->GetIdentityHash()];
  Local<FixedArray> reqs = m->GetModuleRequests();
  for (int i = 0; i < reqs->Length(); i++) {
    Local<ModuleRequest> q = reqs->Get(c, i).As<ModuleRequest>();
    graph_missing(c, resolve_url(base, utf8(isolate, q->GetSpecifier())), seen, missing);
  }
}

static bool graph_ready(Local<Context> c, const std::string& url) {
  std::set<std::string> seen;
  std::vector<std::string> missing;
  graph_missing(c, url, seen, missing);
  return missing.empty();
}

// the module at `url` run and the import's promise settled with it
static void finish_import(Local<Context> c, Local<Promise::Resolver> r, const std::string& url) {
  TryCatch tc(isolate);
  Local<Module> m;
  Local<Value> p;
  if (load_module(c, url, nullptr).ToLocal(&m) && run_module(c, m).ToLocal(&p)) {
    if (p->IsPromise() && p.As<Promise>()->State() == Promise::kRejected) r->Reject(c, p.As<Promise>()->Result()).Check();
    else if (p->IsPromise() && p.As<Promise>()->State() == Promise::kPending) {
      Local<Array> d = Array::New(isolate, 2);
      d->Set(c, 0, r).Check();
      d->Set(c, 1, m->GetModuleNamespace()).Check();
      Local<Function> ok = Function::New(c, dynamic_done, d).ToLocalChecked();
      Local<Function> no = Function::New(c, dynamic_failed, d).ToLocalChecked();
      p.As<Promise>()->Then(c, ok, no).ToLocalChecked();
    } else r->Resolve(c, m->GetModuleNamespace()).Check();
  } else {
    r->Reject(c, tc.HasCaught() ? tc.Exception() : Local<Value>(v8str(isolate, "module failed"))).Check();
  }
}

static MaybeLocal<Promise> import_dynamic(Local<Context> c, Local<Data>, Local<Value> resource, Local<String> spec, Local<FixedArray>) {
  Local<Promise::Resolver> r = Promise::Resolver::New(c).ToLocalChecked();
  std::string base = resource->IsString() ? utf8(isolate, resource) : "";
  if (base.find("://") == std::string::npos) base = page_url(c);
  std::string url = resolve_url(base, utf8(isolate, spec));
  if (graph_ready(c, url)) {
    finish_import(c, r, url);
  } else {
    auto* p = new PendingImport();
    p->c.Reset(isolate, c);
    p->r.Reset(isolate, r);
    p->url = url;
    pending_imports.push_back(p);
  }
  return r->GetPromise();
}

// __fvPump(): the imports of this global whose graphs have now come, run
static void pump_call(const FunctionCallbackInfo<Value>& a) {
  if (pending_imports.empty()) return;
  Local<Context> c = isolate->GetCurrentContext();
  std::vector<PendingImport*> ready;
  for (size_t i = 0; i < pending_imports.size();) {
    PendingImport* p = pending_imports[i];
    if (p->c.Get(isolate) == c && graph_ready(c, p->url)) {
      ready.push_back(p);
      pending_imports.erase(pending_imports.begin() + i);
    } else {
      i++;
    }
  }
  for (PendingImport* p : ready) {
    finish_import(c, p->r.Get(isolate), p->url);
    delete p;
  }
}

// __fvDigest(name, bytes): crypto.subtle.digest's hash (SHA-1, SHA-256, SHA-384, SHA-512) of an ArrayBuffer or view,
// as an ArrayBuffer; undefined for a name it does not know
static void digest_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  std::string name = utf8(iso, a[0]);
  const unsigned char* data = nullptr;
  size_t n = 0;
  std::shared_ptr<BackingStore> keep;
  if (a[1]->IsArrayBufferView()) {
    Local<ArrayBufferView> v = a[1].As<ArrayBufferView>();
    keep = v->Buffer()->GetBackingStore();
    data = static_cast<const unsigned char*>(keep->Data()) + v->ByteOffset();
    n = v->ByteLength();
  } else if (a[1]->IsArrayBuffer()) {
    keep = a[1].As<ArrayBuffer>()->GetBackingStore();
    data = static_cast<const unsigned char*>(keep->Data());
    n = keep->ByteLength();
  } else {
    return;
  }
  unsigned char out[64];
  size_t len = 0;
  if (name == "SHA-1") { SHA1(data, n, out); len = 20; }
  else if (name == "SHA-256") { SHA256(data, n, out); len = 32; }
  else if (name == "SHA-384") { SHA384(data, n, out); len = 48; }
  else if (name == "SHA-512") { SHA512(data, n, out); len = 64; }
  else return;
  Local<ArrayBuffer> buf = ArrayBuffer::New(iso, len);
  memcpy(buf->GetBackingStore()->Data(), out, len);
  a.GetReturnValue().Set(buf);
}

// __fvZlib(mode, format, bytes): WHATWG CompressionStream (mode=0) / DecompressionStream (mode=1)
// using linked zlib ("gzip", "deflate", "deflate-raw"). Returns ArrayBuffer or throws TypeError.
static void zlib_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  Local<Context> c = iso->GetCurrentContext();
  auto throw_type = [&](const char* msg) {
    iso->ThrowException(Exception::TypeError(
        String::NewFromUtf8(iso, msg).ToLocalChecked()));
  };
  int mode = a.Length() > 0 ? a[0]->Int32Value(c).FromMaybe(0) : 0;
  std::string fmt = a.Length() > 1 ? utf8(iso, a[1]) : "";
  int wbits = 0;
  if (fmt == "gzip") wbits = 15 + 16;
  else if (fmt == "deflate") wbits = 15;
  else if (fmt == "deflate-raw") wbits = -15;
  else {
    throw_type("Unsupported compression format");
    return;
  }
  const unsigned char* data = nullptr;
  size_t n = 0;
  std::shared_ptr<BackingStore> keep;
  if (a.Length() > 2 && a[2]->IsArrayBufferView()) {
    Local<ArrayBufferView> v = a[2].As<ArrayBufferView>();
    keep = v->Buffer()->GetBackingStore();
    data = static_cast<const unsigned char*>(keep->Data()) + v->ByteOffset();
    n = v->ByteLength();
  } else if (a.Length() > 2 && a[2]->IsArrayBuffer()) {
    keep = a[2].As<ArrayBuffer>()->GetBackingStore();
    data = static_cast<const unsigned char*>(keep->Data());
    n = keep->ByteLength();
  } else {
    throw_type("Expected ArrayBuffer or ArrayBufferView");
    return;
  }
  constexpr size_t kMaxOut = 64u * 1024u * 1024u;
  constexpr size_t kChunk = 65536u;
  std::vector<unsigned char> out;
  unsigned char tmp[kChunk];
  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  zs.avail_in = static_cast<uInt>(n);
  if (mode == 0) {
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      throw_type("Compression init failed");
      return;
    }
    int ret = Z_OK;
    while (ret == Z_OK) {
      zs.next_out = tmp;
      zs.avail_out = kChunk;
      ret = deflate(&zs, Z_FINISH);
      size_t have = kChunk - zs.avail_out;
      if (out.size() + have > kMaxOut) {
        deflateEnd(&zs);
        throw_type("Compressed output exceeds limit");
        return;
      }
      if (have > 0) out.insert(out.end(), tmp, tmp + have);
    }
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) {
      throw_type("Compression failed");
      return;
    }
  } else {
    if (inflateInit2(&zs, wbits) != Z_OK) {
      throw_type("Decompression init failed");
      return;
    }
    int ret = Z_OK;
    while (ret == Z_OK) {
      zs.next_out = tmp;
      zs.avail_out = kChunk;
      ret = inflate(&zs, Z_FINISH);
      size_t have = kChunk - zs.avail_out;
      if (out.size() + have > kMaxOut) {
        inflateEnd(&zs);
        throw_type("Decompressed output exceeds limit");
        return;
      }
      if (have > 0) out.insert(out.end(), tmp, tmp + have);
      if (ret == Z_BUF_ERROR && have > 0) ret = Z_OK;
    }
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) {
      throw_type("Invalid compressed data");
      return;
    }
  }
  Local<ArrayBuffer> buf = ArrayBuffer::New(iso, out.size());
  if (!out.empty()) memcpy(buf->GetBackingStore()->Data(), out.data(), out.size());
  a.GetReturnValue().Set(buf);
}

// __fvMonotonicNowMs(): monotonic steady_clock timestamp in milliseconds (double, sub-millisecond precision)
static void monotonic_now_ms_call(const FunctionCallbackInfo<Value>& a) {
  using namespace std::chrono;
  const double ms = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
  a.GetReturnValue().Set(ms);
}

// __fvHeap(): the V8 heap's used and total size in bytes, "used/total" (performance.memory reads it)
// __fvDrain(): the microtask checkpoint a browser makes after each task (a timer's callback): promise reactions a
// task queued run before the next task does
static void drain_call(const FunctionCallbackInfo<Value>& a) {
  a.GetIsolate()->PerformMicrotaskCheckpoint();
}

// __fvTask(): a task of the page's begins (a timer's callback among those one run of the host goes through): the
// time a script may run is each task's, as a browser's is, not the whole run's (see `watch`)
static void task_call(const FunctionCallbackInfo<Value>& a);

static void heap_call(const FunctionCallbackInfo<Value>& a) {
  HeapStatistics h;
  a.GetIsolate()->GetHeapStatistics(&h);
  char buf[64];
  snprintf(buf, sizeof buf, "%zu/%zu/%zu", h.used_heap_size(), h.total_heap_size(), h.heap_size_limit());
  a.GetReturnValue().Set(v8str(a.GetIsolate(), buf));
}

// __fvGlobalOf(i): another browsing context's global (a frame's window from its parent, a frame's parent), which a
// script reaches only when the two are of one origin (V8 checks their security tokens)
// __fvScript(code, name): code run as a classic script of the page's, as a <script> element's is (its top-level
// declarations the window's even when it says "use strict", which an indirect eval's are not); what it throws goes
// to the caller
static void script_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* isolate = a.GetIsolate();
  Local<Context> c = isolate->GetCurrentContext();
  if (a.Length() < 1) return;
  Local<String> code;
  if (!a[0]->ToString(c).ToLocal(&code)) return;
  Local<String> name = String::Empty(isolate);
  if (a.Length() > 1 && !a[1]->IsUndefined()) {
    (void)a[1]->ToString(c).ToLocal(&name);
  }
  Local<Script> script;
  Local<Value> out;
  if (code->Length() >= 1024) {
    String::Utf8Value code_utf8(isolate, code);
    String::Utf8Value name_utf8(isolate, name);
    if (*code_utf8 && code_utf8.length() >= 1024) {
      std::string nstr(*name_utf8 ? *name_utf8 : "", *name_utf8 ? (size_t)name_utf8.length() : 0);
      script = fv_compile_cached_utf8(isolate, c, *code_utf8, (size_t)code_utf8.length(), nstr, name);
      if (script.IsEmpty()) return;
      if (script->Run(c).ToLocal(&out)) a.GetReturnValue().Set(out);
      return;
    }
  }
  ScriptOrigin origin(name);
  if (!Script::Compile(c, code, &origin).ToLocal(&script)) return;
  if (script->Run(c).ToLocal(&out)) a.GetReturnValue().Set(out);
}

static void global_of(const FunctionCallbackInfo<Value>& a) {
  int i = a.Length() > 0 ? a[0]->Int32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(-1) : -1;
  if (i < 0 || i >= (int)contexts.size() || contexts[i]->IsEmpty()) return;
  a.GetReturnValue().Set(contexts[i]->Get(a.GetIsolate())->Global());
}

// __fvMakeHTMLAll(handler): document.all's object (HTML §10.6 HTMLAllCollection, ECMAScript Annex B.3.6
// [[IsHTMLDDA]]): V8 counts it undetectable (typeof "undefined", falsy, == null and == undefined, yet === neither), so
// a page's `x === document.all` and `if (document.all)` answer as a browser's (YouTube's Closure sanitiser took every
// undefined binding for document.all while it was undefined, and hid its search results). Called, it asks
// handler("call", arg0); read by index, handler("index", i); a string name its prototype lacks, handler("named", name).
// No Proxy round it: one would hide the undetectable bit.
static Local<Value> html_all_ask(const Local<Object>& self, const char* kind, Local<Value> arg) {
  Isolate* iso = Isolate::GetCurrent();
  Local<Context> ctx = iso->GetCurrentContext();
  Local<Value> h = self->GetInternalField(0).As<Value>();
  if (!h->IsFunction()) return Undefined(iso);
  Local<Value> argv[2] = {String::NewFromUtf8(iso, kind).ToLocalChecked(), arg};
  return h.As<Function>()->Call(ctx, self, 2, argv).FromMaybe(Local<Value>(Undefined(iso)));
}
static void html_all_call(const FunctionCallbackInfo<Value>& a) {
  if (a.Length() == 0) { a.GetReturnValue().SetNull(); return; }
  a.GetReturnValue().Set(html_all_ask(a.This(), "call", a[0]));
}
static Intercepted html_all_index(uint32_t index, const PropertyCallbackInfo<Value>& info) {
  Isolate* iso = info.GetIsolate();
  Local<Value> v = html_all_ask(info.HolderV2(), "index", Integer::NewFromUnsigned(iso, index));
  if (v->IsUndefined()) return Intercepted::kNo;
  info.GetReturnValue().Set(v);
  return Intercepted::kYes;
}
static Intercepted html_all_named(Local<Name> name, const PropertyCallbackInfo<Value>& info) {
  if (!name->IsString()) return Intercepted::kNo;
  Local<Value> v = html_all_ask(info.HolderV2(), "named", name);
  if (v->IsUndefined()) return Intercepted::kNo;
  info.GetReturnValue().Set(v);
  return Intercepted::kYes;
}
static void make_html_all(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  Local<ObjectTemplate> t = ObjectTemplate::New(iso);
  t->SetInternalFieldCount(1);
  t->MarkAsUndetectable();
  t->SetCallAsFunctionHandler(html_all_call);
  t->SetHandler(IndexedPropertyHandlerConfiguration(html_all_index));
  t->SetHandler(NamedPropertyHandlerConfiguration(html_all_named, nullptr, nullptr, nullptr, nullptr, Local<Value>(),
      static_cast<PropertyHandlerFlags>(static_cast<int>(PropertyHandlerFlags::kNonMasking) |
                                        static_cast<int>(PropertyHandlerFlags::kOnlyInterceptStrings))));
  Local<Object> o;
  if (!t->NewInstance(iso->GetCurrentContext()).ToLocal(&o)) return;
  o->SetInternalField(0, a.Length() > 0 ? a[0] : Local<Value>(Undefined(iso)));
  a.GetReturnValue().Set(o);
}

// __fvHand(v) and __fvTake(id): a value handed from one browsing context to another with a message (the ports a
// postMessage transfers to a frame of another origin), kept here by number until the receiver takes it
static std::map<int, Global<Value>*> handed;
static int handed_next = 0;
static void hand_call(const FunctionCallbackInfo<Value>& a) {
  if (a.Length() < 1) return;
  int id = ++handed_next;
  handed[id] = new Global<Value>(a.GetIsolate(), a[0]);
  a.GetReturnValue().Set(id);
}
static void take_call(const FunctionCallbackInfo<Value>& a) {
  int id = a.Length() > 0 ? a[0]->Int32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(0) : 0;
  auto it = handed.find(id);
  if (it == handed.end()) return;
  a.GetReturnValue().Set(it->second->Get(a.GetIsolate()));
  delete it->second;
  handed.erase(it);
}

// __fvWithCaller(f): a function that calls f with the global of the script calling it first, then its own arguments
// (window.postMessage: a message's source is the window whose script posted it, which may be a same-origin frame's
// calling its parent's method; the native function's incumbent context is that caller's)
static void with_caller_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* isolate = a.GetIsolate();
  Local<Context> here = isolate->GetCurrentContext();
  Local<Function> f = a.Data().As<Function>();
  Local<Context> inc = isolate->GetIncumbentContext();
  std::vector<Local<Value>> args;
  args.push_back(inc.IsEmpty() ? Local<Value>(here->Global()) : Local<Value>(inc->Global()));
  for (int i = 0; i < a.Length(); i++) args.push_back(a[i]);
  Local<Value> out;
  if (f->Call(here, a.This(), (int)args.size(), args.data()).ToLocal(&out)) a.GetReturnValue().Set(out);
}
static void with_caller(const FunctionCallbackInfo<Value>& a) {
  if (a.Length() < 1 || !a[0]->IsFunction()) return;
  Isolate* isolate = a.GetIsolate();
  Local<Function> g;
  if (Function::New(isolate->GetCurrentContext(), with_caller_call, a[0]).ToLocal(&g)) a.GetReturnValue().Set(g);
}

// __fvModulesReset(): a new page starts with no modules (each page runs its own)
static void modules_reset(const FunctionCallbackInfo<Value>&) {
  for (auto* p : pending_imports) delete p;
  pending_imports.clear();
  for (auto& kv : modules) delete kv.second;
  modules.clear();
  module_urls.clear();
}

static void import_meta(Local<Context> c, Local<Module> m, Local<Object> meta) {
  meta->Set(c, v8str(isolate, "url"), v8str(isolate, module_urls[m->GetIdentityHash()])).Check();
}

// Promises rejected with no handler (a page's async code that failed): told to the host as script errors once the
// script run that left them ends, as a browser's console shows them
static std::vector<std::pair<Global<Promise>*, std::string>> unhandled;

static void promise_rejected(PromiseRejectMessage m) {
  Local<Promise> p = m.GetPromise();
  if (m.GetEvent() == kPromiseRejectWithNoHandler) {
    Local<Value> v = m.GetValue();
    std::string text;
    if (!v.IsEmpty() && v->IsObject()) {
      Local<Value> st;
      if (v.As<Object>()->Get(isolate->GetCurrentContext(), v8str(isolate, "stack")).ToLocal(&st) && st->IsString()) text = utf8(isolate, st);
    }
    if (text.empty() && !v.IsEmpty()) text = utf8(isolate, v);
    unhandled.push_back({new Global<Promise>(isolate, p), text});
  } else if (m.GetEvent() == kPromiseHandlerAddedAfterReject) {
    for (auto it = unhandled.begin(); it != unhandled.end(); ++it) {
      if (it->first->Get(isolate) == p) { delete it->first; unhandled.erase(it); break; }
    }
  }
}

static void report_unhandled() {
  auto list = std::move(unhandled);
  unhandled.clear();
  for (auto& u : list) {
    delete u.first;
    moonbit_string_t r = call_host(23, 2, 0, mbstr("Uncaught (in promise) " + u.second), mbstr(""));
    moonbit_decref(r);
  }
}

extern "C" void fv_js_start_ref(fv_host_t fn) {
  host_fn = fn;
  fv_v8_ensure_platform();
  if (isolate != nullptr) {
    fv_js_new_context();
    return;
  }
  Isolate::CreateParams params;
  params.array_buffer_allocator = ArrayBuffer::Allocator::NewDefaultAllocator();
  isolate = Isolate::New(params);
  isolate->SetHostImportModuleDynamicallyCallback(import_dynamic);
  isolate->SetPromiseRejectCallback(promise_rejected);
  isolate->SetHostInitializeImportMetaObjectCallback(import_meta);
  {
    Isolate::Scope is(isolate);
    HandleScope hs(isolate);
    g_str_nul.Set(isolate, String::NewFromUtf8(isolate, "\0", NewStringType::kInternalized, 1).ToLocalChecked());
    g_str_zero.Set(isolate, String::NewFromUtf8Literal(isolate, "0", NewStringType::kInternalized));
    g_str_one.Set(isolate, String::NewFromUtf8Literal(isolate, "1", NewStringType::kInternalized));
  }
  fv_js_new_context();
}

static Local<ObjectTemplate> get_world_global_template(Isolate* iso) {
  if (!g_world_global_tmpl.IsEmpty()) return g_world_global_tmpl.Get(iso);
  Local<ObjectTemplate> global = ObjectTemplate::New(iso);
  global->Set(iso, "__fv", FunctionTemplate::New(iso, host_call));
  global->Set(iso, "__fvi", FunctionTemplate::New(iso, host_int_call));
  global->Set(iso, "__fvDigest", FunctionTemplate::New(iso, digest_call));
  global->Set(iso, "__fvZlib", FunctionTemplate::New(iso, zlib_call));
  global->Set(iso, "__fvDrain", FunctionTemplate::New(iso, drain_call));
  global->Set(iso, "__fvMonotonicNowMs", FunctionTemplate::New(iso, monotonic_now_ms_call));
  fv_crypto_install(iso, global);
  g_world_global_tmpl.Reset(iso, global);
  return global;
}

extern "C" int32_t fv_js_world_create(int32_t ctx_id, const char* world_key, int32_t key_len);
extern "C" int32_t fv_js_world_destroy(int32_t ctx_id, const char* world_key, int32_t key_len);
extern "C" void fv_js_world_eval_utf8(int32_t ctx_id, const char* world_key, int32_t key_len,
                                      const char* src_utf8, int32_t src_len,
                                      fv_utf8_emit_t emit, void* ctx);

static void world_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* iso = a.GetIsolate();
  Local<Context> cur = iso->GetCurrentContext();
  if (a.Length() < 2) { a.GetReturnValue().Set(String::Empty(iso)); return; }
  String::Utf8Value op(iso, a[0]);
  String::Utf8Value key(iso, a[1]);
  if (!*op || !*key) { a.GetReturnValue().Set(String::Empty(iso)); return; }
  int32_t cid = current_ctx_id();
  if (strcmp(*op, "create") == 0) {
    int32_t ok = fv_js_world_create(cid, *key, key.length());
    a.GetReturnValue().Set(String::NewFromUtf8Literal(iso, ok ? "1" : "0"));
    return;
  }
  if (strcmp(*op, "destroy") == 0) {
    int32_t ok = fv_js_world_destroy(cid, *key, key.length());
    a.GetReturnValue().Set(String::NewFromUtf8Literal(iso, ok ? "1" : "0"));
    return;
  }
  if (strcmp(*op, "exists") == 0) {
    std::string mk = world_map_key(cid, *key, key.length());
    bool ex = g_worlds.find(mk) != g_worlds.end();
    a.GetReturnValue().Set(String::NewFromUtf8Literal(iso, ex ? "1" : "0"));
    return;
  }
  if (strcmp(*op, "eval") == 0 && a.Length() >= 3) {
    String::Utf8Value src(iso, a[2]);
    std::string res;
    auto emit_str = [](void* p, const char* data, int32_t len) {
      if (data && len > 0) static_cast<std::string*>(p)->assign(data, (size_t)len);
    };
    fv_js_world_eval_utf8(cid, *key, key.length(), *src ? *src : "", src.length(), emit_str, &res);
    Local<String> out;
    if (String::NewFromUtf8(iso, res.data(), NewStringType::kNormal, (int)res.size()).ToLocal(&out)) {
      a.GetReturnValue().Set(out);
    } else {
      a.GetReturnValue().Set(String::Empty(iso));
    }
    return;
  }
  (void)cur;
  a.GetReturnValue().Set(String::Empty(iso));
}

static Local<ObjectTemplate> get_global_template(Isolate* iso) {
  if (!g_global_tmpl.IsEmpty()) return g_global_tmpl.Get(iso);
  Local<ObjectTemplate> global = ObjectTemplate::New(iso);
  global->Set(iso, "__fv", FunctionTemplate::New(iso, host_call));
  global->Set(iso, "__fvi", FunctionTemplate::New(iso, host_int_call));
  global->Set(iso, "__fvModule", FunctionTemplate::New(iso, module_call));
  global->Set(iso, "__fvModulesReset", FunctionTemplate::New(iso, modules_reset));
  global->Set(iso, "__fvImportMap", FunctionTemplate::New(iso, import_map_call));
  global->Set(iso, "__fvDigest", FunctionTemplate::New(iso, digest_call));
  global->Set(iso, "__fvZlib", FunctionTemplate::New(iso, zlib_call));
  global->Set(iso, "__fvHeap", FunctionTemplate::New(iso, heap_call));
  global->Set(iso, "__fvDrain", FunctionTemplate::New(iso, drain_call));
  global->Set(iso, "__fvTask", FunctionTemplate::New(iso, task_call));
  global->Set(iso, "__fvPump", FunctionTemplate::New(iso, pump_call));
  global->Set(iso, "__fvGlobalOf", FunctionTemplate::New(iso, global_of));
  global->Set(iso, "__fvScript", FunctionTemplate::New(iso, script_call));
  global->Set(iso, "__fvHand", FunctionTemplate::New(iso, hand_call));
  global->Set(iso, "__fvTake", FunctionTemplate::New(iso, take_call));
  global->Set(iso, "__fvWithCaller", FunctionTemplate::New(iso, with_caller));
  global->Set(iso, "__fvMakeHTMLAll", FunctionTemplate::New(iso, make_html_all));
  global->Set(iso, "__fvWorld", FunctionTemplate::New(iso, world_call));
  global->Set(iso, "__fvMonotonicNowMs", FunctionTemplate::New(iso, monotonic_now_ms_call));
  fv_crypto_install(iso, global);
  fv_worker_install_main_bindings(iso, global);
  g_global_tmpl.Reset(iso, global);
  return global;
}

// A page's own global: a new page starts in a new context, so nothing the last page left (its timers' closures, its
// objects, its modules) reaches it
extern "C" void fv_js_new_context(void) {
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  destroy_worlds_for_ctx(-1);
  for (auto& kv : modules) delete kv.second;
  modules.clear();
  module_urls.clear();
  import_map.clear();
  for (auto& u : unhandled) delete u.first;
  unhandled.clear();
  Local<Context> c = Context::New(isolate, nullptr, get_global_template(isolate));
  install_dom_ver(isolate, c);
  for (auto* g : contexts) {
    g->Reset();
    delete g;
  }
  contexts.clear();
  context = new Global<Context>(isolate, c);
  contexts.push_back(context);
}

// A frame's own global, beside the page's: its index (scripts run in it once used)
extern "C" int32_t fv_js_add_context(void) {
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  Local<Context> c = Context::New(isolate, nullptr, get_global_template(isolate));
  install_dom_ver(isolate, c);
  contexts.push_back(new Global<Context>(isolate, c));
  return (int32_t)contexts.size() - 1;
}

// Which global the scripts run in from now
extern "C" void fv_js_use(int32_t i) {
  if (i >= 0 && i < (int32_t)contexts.size() && !contexts[i]->IsEmpty()) context = contexts[i];
}

// A frame's global let go (its iframe gone): kept only by what still holds its objects, then collected
extern "C" void fv_js_drop_context(int32_t i) {
  if (i > 0 && i < (int32_t)contexts.size() && contexts[i] != context && !contexts[i]->IsEmpty()) {
    destroy_worlds_for_ctx(i);
    contexts[i]->Reset();
    // (its modules too: each holds its global, which would never be collected)
    std::string prefix = std::to_string(i) + " ";
    for (auto it = modules.begin(); it != modules.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) {
        it->second->Reset();
        delete it->second;
        it = modules.erase(it);
      } else {
        ++it;
      }
    }
    isolate->ContextDisposedNotification();
  }
}

// A global's origin, as V8's security token: globals of one origin reach each other's objects; of two, not
extern "C" void fv_js_set_origin(int32_t i, moonbit_string_t origin) {
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  if (i >= 0 && i < (int32_t)contexts.size() && !contexts[i]->IsEmpty()) {
    Local<Context> c = contexts[i]->Get(isolate);
    // (internalized: V8 compares tokens as objects, so one origin must be one string for its contexts to match)
    c->SetSecurityToken(String::NewFromTwoByte(isolate, reinterpret_cast<const uint16_t*>(origin), NewStringType::kInternalized, Moonbit_array_length(origin)).ToLocalChecked());
  }
  moonbit_decref(origin);
}

// A script that runs too long (a page's loop that never ends) is stopped, as a browser offers to: the outermost run
// has a deadline (FV_SCRIPT_MS, 10 s), and a watcher thread ends the script past it.
static std::atomic<int64_t> deadline{0};
static int depth = 0;

static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::atomic<int64_t> g_script_limit_ms{-1};

extern "C" void fv_js_set_script_ms(int32_t ms) {
  g_script_limit_ms.store((int64_t)ms, std::memory_order_relaxed);
}

static int64_t script_limit_ms() {
  int64_t cached = g_script_limit_ms.load(std::memory_order_relaxed);
  if (cached >= 0) return cached;
  const char* limit = getenv("FV_SCRIPT_MS");
  int64_t v = limit ? atoll(limit) : 10000;
  g_script_limit_ms.store(v, std::memory_order_relaxed);
  return v;
}

static void task_call(const FunctionCallbackInfo<Value>& a) {
  if (deadline.load() != 0) deadline.store(now_ms() + script_limit_ms());
}

static void watch() {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      int64_t d = deadline.load();
      if (d != 0 && now_ms() > d) {
        deadline.store(0);
        // where it was, for FV_SCRIPT_LOG, then stopped
        isolate->RequestInterrupt([](Isolate* i, void*) {
          if (getenv("FV_SCRIPT_LOG")) {
            HandleScope hs(i);
            Local<StackTrace> st = StackTrace::CurrentStackTrace(i, 12);
            fprintf(stderr, "script stopped after its time:\n");
            for (int k = 0; k < st->GetFrameCount(); k++) {
              Local<StackFrame> f = st->GetFrame(i, k);
              String::Utf8Value fn(i, f->GetFunctionName()), file(i, f->GetScriptName());
              fprintf(stderr, "  %s %s:%d:%d\n", *fn ? *fn : "?", *file ? *file : "?", f->GetLineNumber(), f->GetColumn());
            }
          }
          i->TerminateExecution();
        }, nullptr);
      }
    }
  }).detach();
}

extern "C" moonbit_string_t fv_js_run(moonbit_string_t source) {
  watch();
  if (depth == 0) {
    deadline.store(now_ms() + script_limit_ms());
  }
  depth++;
  struct Done {
    ~Done() {
      depth--;
      if (depth == 0) {
        deadline.store(0);
        isolate->CancelTerminateExecution();
        // the run is over: what WeakRefs were made or read in it may be collected now (a browser does this after
        // each task), and the engine's own tasks (the collector's steps, finalization callbacks) run
        isolate->ClearKeptObjects();
        // its tasks may run scripts (a WebAssembly module compiled in the background, instantiated), in the page's
        // global
        Isolate::Scope is(isolate);
        HandleScope hs(isolate);
        Context::Scope cs(context->Get(isolate));
        while (platform::PumpMessageLoop(default_platform.get(), isolate)) {
        }
      }
    }
  } done;
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  Local<Context> c = context->Get(isolate);
  Context::Scope cs(c);
  // what the last runs left rejected, told now (its microtasks ran after it)
  if (depth == 1 && !unhandled.empty()) report_unhandled();
  TryCatch tc(isolate);
  Local<String> src = String::NewFromTwoByte(isolate, reinterpret_cast<const uint16_t*>(source), NewStringType::kNormal, Moonbit_array_length(source)).ToLocalChecked();
  moonbit_decref(source);
  Local<Script> script;
  Local<Value> out;
  if (Script::Compile(c, src).ToLocal(&script) && script->Run(c).ToLocal(&out)) {
    Local<String> s;
    if (out->ToString(c).ToLocal(&s)) return to_mb(isolate, s);
    return moonbit_make_string(0, 0);
  }
  Local<String> e;
  if (tc.HasCaught() && tc.Exception()->ToString(c).ToLocal(&e)) {
    Local<String> prefixed = String::Concat(isolate, String::NewFromUtf8Literal(isolate, "!error: "), e);
    // where it was thrown (an exception with no stack of its own, a stack overflow, still has a place)
    Local<Message> m = tc.Message();
    if (!m.IsEmpty()) {
      int line = m->GetLineNumber(c).FromMaybe(0);
      int col = m->GetStartColumn(c).FromMaybe(0);
      Local<String> src;
      std::string where = " at line " + std::to_string(line) + ":" + std::to_string(col);
      if (m->GetSourceLine(c).ToLocal(&src)) {
        String::Utf8Value text(isolate, src);
        std::string t(*text ? *text : "");
        size_t from = col > 80 ? col - 80 : 0;
        where += " near: " + t.substr(from, 160);
      }
      prefixed = String::Concat(isolate, prefixed, String::NewFromUtf8(isolate, where.c_str()).ToLocalChecked());
    }
    return to_mb(isolate, prefixed);
  }
  return moonbit_make_string(0, 0);
}

struct CachedShortScript {
  std::string src;
  Global<UnboundScript> script;
};
static constexpr size_t FV_SHORT_SCRIPT_CACHE_MAX = 32;
static std::vector<CachedShortScript> g_short_script_cache;

static Local<Script> fv_compile_short_cached_utf8(
    Isolate* iso, Local<Context> c, const char* src_utf8, int32_t src_len) {
  for (size_t i = 0; i < g_short_script_cache.size(); i++) {
    auto& e = g_short_script_cache[i];
    if ((int32_t)e.src.size() == src_len && memcmp(e.src.data(), src_utf8, (size_t)src_len) == 0) {
      return e.script.Get(iso)->BindToCurrentContext();
    }
  }
  Local<String> v8_src;
  if (!String::NewFromUtf8(iso, src_utf8, NewStringType::kNormal, src_len).ToLocal(&v8_src)) {
    return Local<Script>();
  }
  ScriptCompiler::Source source(v8_src);
  Local<UnboundScript> unbound;
  if (!ScriptCompiler::CompileUnboundScript(iso, &source).ToLocal(&unbound)) {
    return Local<Script>();
  }
  if (g_short_script_cache.size() >= FV_SHORT_SCRIPT_CACHE_MAX) {
    g_short_script_cache.erase(g_short_script_cache.begin());
  }
  CachedShortScript entry;
  entry.src.assign(src_utf8, (size_t)src_len);
  entry.script.Reset(iso, unbound);
  g_short_script_cache.push_back(std::move(entry));
  return unbound->BindToCurrentContext();
}

extern "C" void fv_js_run_utf8(const char* src_utf8, int32_t src_len, fv_utf8_emit_t emit, void* ctx) {
  watch();
  if (depth == 0) {
    deadline.store(now_ms() + script_limit_ms());
  }
  depth++;
  struct Done {
    ~Done() {
      depth--;
      if (depth == 0) {
        deadline.store(0);
        isolate->CancelTerminateExecution();
        isolate->ClearKeptObjects();
        Isolate::Scope is(isolate);
        HandleScope hs(isolate);
        Context::Scope cs(context->Get(isolate));
        while (platform::PumpMessageLoop(default_platform.get(), isolate)) {
        }
      }
    }
  } done;
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  Local<Context> c = context->Get(isolate);
  Context::Scope cs(c);
  if (depth == 1 && !unhandled.empty()) report_unhandled();
  TryCatch tc(isolate);
  Local<Script> script;
  Local<Value> out;
  bool compiled = false;
  if (src_utf8 && src_len >= 4096) {
    static const std::string empty_name;
    script = fv_compile_cached_utf8(isolate, c, src_utf8, (size_t)src_len, empty_name, String::Empty(isolate));
    compiled = !script.IsEmpty();
  } else if (src_utf8 && src_len > 0 && src_len <= 256) {
    script = fv_compile_short_cached_utf8(isolate, c, src_utf8, src_len);
    compiled = !script.IsEmpty();
  } else {
    Local<String> src = String::NewFromUtf8(
        isolate, src_utf8 ? src_utf8 : "", NewStringType::kNormal, src_len > 0 ? src_len : 0).ToLocalChecked();
    compiled = Script::Compile(c, src).ToLocal(&script);
  }
  if (compiled && script->Run(c).ToLocal(&out)) {
    Local<String> s;
    if (out->ToString(c).ToLocal(&s)) {
      String::Utf8Value utf8(isolate, s);
      emit(ctx, *utf8 ? *utf8 : "", utf8.length());
      return;
    }
    emit(ctx, "", 0);
    return;
  }
  Local<String> e;
  if (tc.HasCaught() && tc.Exception()->ToString(c).ToLocal(&e)) {
    String::Utf8Value err_utf8(isolate, e);
    std::string msg = "!error: ";
    if (*err_utf8) msg.append(*err_utf8, (size_t)err_utf8.length());
    Local<Message> m = tc.Message();
    if (!m.IsEmpty()) {
      int line = m->GetLineNumber(c).FromMaybe(0);
      int col = m->GetStartColumn(c).FromMaybe(0);
      Local<String> line_src;
      msg += " at line " + std::to_string(line) + ":" + std::to_string(col);
      if (m->GetSourceLine(c).ToLocal(&line_src)) {
        String::Utf8Value text(isolate, line_src);
        std::string t(*text ? *text : "");
        size_t from = col > 80 ? col - 80 : 0;
        msg += " near: " + t.substr(from, 160);
      }
    }
    emit(ctx, msg.data(), (int32_t)msg.size());
    return;
  }
  emit(ctx, "", 0);
}

// The host's key: what its own calls into a page name the host-only entry points with (__fvH(key)), random for the
// process so that a page's scripts, which never see the host's code, cannot make it up
extern "C" moonbit_string_t fv_js_host_key(void) {
  static char key[33] = {0};
  if (!key[0]) {
    unsigned char b[16];
    arc4random_buf(b, sizeof b);
    for (int i = 0; i < 16; i++) snprintf(key + 2 * i, 3, "%02x", b[i]);
  }
  moonbit_string_t s = moonbit_make_string(32, 0);
  for (int i = 0; i < 32; i++) s[i] = (uint16_t)key[i];
  return s;
}

// (FV_PROF) A full collection, as under memory pressure: what the heap holds after it is what scripts keep
extern "C" void fv_js_collect(void) {
  isolate->LowMemoryNotification();
}

// (FV_PROF) How many globals the heap holds, and of them how many were let go but are still reached
extern "C" int32_t fv_js_contexts(void) {
  HeapStatistics h;
  isolate->GetHeapStatistics(&h);
  return (int32_t)(h.number_of_native_contexts() * 1000 + h.number_of_detached_contexts());
}

extern "C" int64_t fv_js_heap(void) {
  HeapStatistics h;
  isolate->GetHeapStatistics(&h);
  return (int64_t)h.used_heap_size();
}

// --- Real V8 Isolated Worlds (independent v8::Context per (ctx_id, world_key)) ---

extern "C" int32_t fv_js_world_create(int32_t ctx_id, const char* world_key, int32_t key_len) {
  if (!isolate || !world_key || key_len <= 0) return 0;
  std::string mk = world_map_key(ctx_id, world_key, key_len);
  if (g_worlds.find(mk) != g_worlds.end()) return 1;
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  Local<Context> c = Context::New(isolate, nullptr, get_world_global_template(isolate));
  if (c.IsEmpty()) return 0;
  install_dom_ver(isolate, c);
  std::string tok = "fv-world:" + mk;
  Local<String> v8_tok;
  if (String::NewFromUtf8(isolate, tok.data(), NewStringType::kInternalized, (int)tok.size()).ToLocal(&v8_tok)) {
    c->SetSecurityToken(v8_tok);
  }
  g_worlds[mk] = new Global<Context>(isolate, c);
  return 1;
}

extern "C" void fv_js_world_eval_utf8(int32_t ctx_id, const char* world_key, int32_t key_len,
                                      const char* src_utf8, int32_t src_len,
                                      fv_utf8_emit_t emit, void* ctx) {
  if (!isolate || !world_key || key_len <= 0) {
    emit(ctx, "!error: invalid world", 21);
    return;
  }
  std::string mk = world_map_key(ctx_id, world_key, key_len);
  auto it = g_worlds.find(mk);
  if (it == g_worlds.end() || it->second->IsEmpty()) {
    emit(ctx, "!error: world not found", 23);
    return;
  }
  watch();
  if (depth == 0) {
    deadline.store(now_ms() + script_limit_ms());
  }
  depth++;
  struct Done {
    ~Done() {
      depth--;
      if (depth == 0) {
        deadline.store(0);
        isolate->CancelTerminateExecution();
        isolate->ClearKeptObjects();
        Isolate::Scope is(isolate);
        HandleScope hs(isolate);
        Context::Scope cs(context->Get(isolate));
        while (platform::PumpMessageLoop(default_platform.get(), isolate)) {
        }
      }
    }
  } done;
  Isolate::Scope is(isolate);
  HandleScope hs(isolate);
  Local<Context> c = it->second->Get(isolate);
  Context::Scope cs(c);
  TryCatch tc(isolate);
  Local<String> src;
  if (!String::NewFromUtf8(isolate, src_utf8 ? src_utf8 : "", NewStringType::kNormal, src_len > 0 ? src_len : 0).ToLocal(&src)) {
    emit(ctx, "!error: bad utf8", 16);
    return;
  }
  Local<Script> script;
  Local<Value> out;
  if (Script::Compile(c, src).ToLocal(&script) && script->Run(c).ToLocal(&out)) {
    isolate->PerformMicrotaskCheckpoint();
    Local<String> s;
    if (out->ToString(c).ToLocal(&s)) {
      String::Utf8Value utf8(isolate, s);
      emit(ctx, *utf8 ? *utf8 : "", utf8.length());
      return;
    }
    emit(ctx, "", 0);
    return;
  }
  Local<String> e;
  if (tc.HasCaught() && tc.Exception()->ToString(c).ToLocal(&e)) {
    String::Utf8Value err_utf8(isolate, e);
    std::string msg = "!error: ";
    if (*err_utf8) msg.append(*err_utf8, (size_t)err_utf8.length());
    Local<Message> m = tc.Message();
    if (!m.IsEmpty()) {
      int line = m->GetLineNumber(c).FromMaybe(0);
      int col = m->GetStartColumn(c).FromMaybe(0);
      msg += " at line " + std::to_string(line) + ":" + std::to_string(col);
    }
    emit(ctx, msg.data(), (int32_t)msg.size());
    return;
  }
  emit(ctx, "", 0);
}

extern "C" int32_t fv_js_world_destroy(int32_t ctx_id, const char* world_key, int32_t key_len) {
  if (!isolate || !world_key || key_len <= 0) return 0;
  std::string mk = world_map_key(ctx_id, world_key, key_len);
  auto it = g_worlds.find(mk);
  if (it == g_worlds.end()) return 0;
  it->second->Reset();
  delete it->second;
  g_worlds.erase(it);
  isolate->ContextDisposedNotification();
  return 1;
}

extern "C" int32_t fv_js_world_count(int32_t ctx_id) {
  if (ctx_id < 0) return (int32_t)g_worlds.size();
  std::string prefix = std::to_string(ctx_id) + "\x01";
  int32_t n = 0;
  for (const auto& kv : g_worlds) {
    if (kv.first.compare(0, prefix.size(), prefix) == 0) n++;
  }
  return n;
}
