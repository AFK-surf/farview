// Farview Wave W1: Real OS-thread + independent v8::Isolate Web Worker runtime.
// Supports up to FV_MAX_WORKERS (16) concurrent DedicatedWorker threads with
// v8::ValueSerializer / v8::ValueDeserializer structured cloning and zero-copy
// Transferable ArrayBuffer handoff across Isolates.

#include "v8_worker.h"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace v8;

// Defined in native/js/crypto.cc (Isolate-independent OpenSSL/EVP WebCrypto backend).
void fv_crypto_install(Isolate* i, Local<ObjectTemplate> global);

namespace {

static uint64_t get_os_tid() {
#ifdef __APPLE__
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#elif defined(__linux__)
  return static_cast<uint64_t>(syscall(SYS_gettid));
#else
  return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

static double steady_now_ms() {
  using namespace std::chrono;
  return duration_cast<duration<double, std::milli>>(
             steady_clock::now().time_since_epoch())
      .count();
}

// Dedicated per-slot thread-safe ArrayBuffer allocator.
// Lives in static slot storage so transferred BackingStores remain safely
// freeable even if the originating worker terminates before the receiver drops
// the ArrayBuffer.
class FvSlotArrayBufferAllocator final : public ArrayBuffer::Allocator {
 public:
  void* Allocate(size_t length) override {
    void* p = std::calloc(length ? length : 1, 1);
    return p;
  }
  void* AllocateUninitialized(size_t length) override {
    void* p = std::malloc(length ? length : 1);
    return p;
  }
  void Free(void* data, size_t) override { std::free(data); }
};

struct FvWorkerMessage {
  int32_t kind = FV_WORKER_MSG_DATA;  // 1=v8 serialized, 2=error utf8, 3=raw utf8
  std::vector<uint8_t> serialized;
  std::vector<std::shared_ptr<BackingStore>> transfers;
  std::string text;  // UTF-8 representation for C/Pion polling & error messages
};

struct FvWorkerTimer {
  int32_t id = 0;
  double fire_at_ms = 0.0;
  double interval_ms = 0.0;
  bool repeat = false;
  Global<Function> cb;
};

struct FvWorkerSlot {
  std::mutex mu;
  std::condition_variable cv;
  std::thread thread;

  std::atomic<bool> in_use{false};
  // Protected by `g_pool_mu`: prevents concurrent double-terminate races while join() runs.
  bool terminating = false;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> exited{false};
  std::atomic<bool> ready{false};
  std::atomic<uint64_t> os_tid{0};

  // Protected by `mu`: pointer valid only while the worker thread's Isolate is alive.
  Isolate* worker_isolate = nullptr;

  std::string script;
  std::string url;
  double start_ms = 0.0;

  std::deque<FvWorkerMessage> inbound;
  std::deque<FvWorkerMessage> outbound;

  // Worker-thread-only timer & virtual module state (accessed strictly on the worker OS thread).
  int32_t next_timer_id = 1;
  std::vector<FvWorkerTimer> timers;
  std::unordered_map<std::string, std::string> vfs;
  std::unordered_map<std::string, Global<Module>*> mod_cache;
  std::unordered_map<int, std::string> mod_urls;

  FvSlotArrayBufferAllocator allocator;
};

static FvWorkerSlot g_slots[FV_MAX_WORKERS];
static std::mutex g_pool_mu;
static std::atomic<int32_t> g_active_count{0};
static std::once_flag g_atexit_once;

// Convert a V8 value to a compact UTF-8 text summary (for C/Pion fv_worker_poll).
static std::string v8_value_to_utf8_summary(Isolate* iso, Local<Context> ctx, Local<Value> val) {
  if (val.IsEmpty() || val->IsUndefined()) return "undefined";
  if (val->IsNull()) return "null";
  if (val->IsString()) {
    String::Utf8Value u(iso, val);
    return *u ? std::string(*u, static_cast<size_t>(u.length())) : std::string();
  }
  TryCatch tc(iso);
  Local<String> json_str;
  if (JSON::Stringify(ctx, val).ToLocal(&json_str)) {
    String::Utf8Value u(iso, json_str);
    return *u ? std::string(*u, static_cast<size_t>(u.length())) : std::string();
  }
  tc.Reset();
  Local<String> s;
  if (val->ToString(ctx).ToLocal(&s)) {
    String::Utf8Value u(iso, s);
    return *u ? std::string(*u, static_cast<size_t>(u.length())) : std::string();
  }
  return "";
}

// Serialize `val` with optional `transfer_arg` (Array of ArrayBuffers) using v8::ValueSerializer.
// Detaches transferred ArrayBuffers on success.
static bool serialize_v8_message(Isolate* iso, Local<Context> ctx,
                                 Local<Value> val, Local<Value> transfer_arg,
                                 FvWorkerMessage& out, std::string& err_out) {
  std::vector<Local<ArrayBuffer>> transfer_abs;
  if (!transfer_arg.IsEmpty() && !transfer_arg->IsUndefined() && !transfer_arg->IsNull()) {
    Local<Array> arr;
    if (transfer_arg->IsArray()) {
      arr = transfer_arg.As<Array>();
    } else if (transfer_arg->IsObject()) {
      Local<Value> tprop;
      if (transfer_arg.As<Object>()
              ->Get(ctx, String::NewFromUtf8Literal(iso, "transfer"))
              .ToLocal(&tprop) &&
          tprop->IsArray()) {
        arr = tprop.As<Array>();
      }
    }
    if (!arr.IsEmpty()) {
      uint32_t len = arr->Length();
      for (uint32_t i = 0; i < len; ++i) {
        Local<Value> elem;
        if (!arr->Get(ctx, i).ToLocal(&elem)) continue;
        if (!elem->IsArrayBuffer()) {
          err_out = "DataCloneError: transfer list element is not an ArrayBuffer";
          return false;
        }
        Local<ArrayBuffer> ab = elem.As<ArrayBuffer>();
        if (ab->WasDetached()) {
          err_out = "DataCloneError: ArrayBuffer is already detached";
          return false;
        }
        if (!ab->IsDetachable()) {
          err_out = "DataCloneError: ArrayBuffer is not detachable";
          return false;
        }
        // Check duplicates
        for (const auto& prev : transfer_abs) {
          if (prev->StrictEquals(ab)) {
            err_out = "DataCloneError: duplicate ArrayBuffer in transfer list";
            return false;
          }
        }
        transfer_abs.push_back(ab);
      }
    }
  }

  // Capture text summary BEFORE detaching transferred ArrayBuffers.
  out.text = v8_value_to_utf8_summary(iso, ctx, val);

  ValueSerializer serializer(iso);
  serializer.WriteHeader();

  std::vector<std::shared_ptr<BackingStore>> stores;
  stores.reserve(transfer_abs.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(transfer_abs.size()); ++i) {
    stores.push_back(transfer_abs[i]->GetBackingStore());
    serializer.TransferArrayBuffer(i, transfer_abs[i]);
  }

  TryCatch tc(iso);
  Maybe<bool> ok = serializer.WriteValue(ctx, val);
  if (ok.IsNothing() || !ok.FromJust()) {
    if (tc.HasCaught()) {
      String::Utf8Value u(iso, tc.Exception());
      err_out = *u ? std::string(*u, static_cast<size_t>(u.length())) : "DataCloneError";
    } else {
      err_out = "DataCloneError: value could not be cloned";
    }
    return false;
  }

  // Detach transferred ArrayBuffers on sender side (true zero-copy handoff).
  for (auto& ab : transfer_abs) {
    (void)ab->Detach(Local<Value>()).FromMaybe(false);
  }

  auto released = serializer.Release();
  out.kind = FV_WORKER_MSG_DATA;
  if (released.first && released.second > 0) {
    out.serialized.assign(released.first, released.first + released.second);
  }
  if (released.first) {
    std::free(released.first);
  }
  out.transfers = std::move(stores);
  return true;
}

// Deserialize a FvWorkerMessage into the target Isolate/Context.
static MaybeLocal<Value> deserialize_v8_message(Isolate* iso, Local<Context> ctx,
                                                const FvWorkerMessage& msg) {
  if (msg.kind == 3) {
    // Raw UTF-8 string from C/Pion fv_worker_post
    return String::NewFromUtf8(iso, msg.text.data(), NewStringType::kNormal,
                               static_cast<int>(msg.text.size()))
        .ToLocalChecked();
  }
  if (msg.serialized.empty()) {
    return Undefined(iso);
  }
  ValueDeserializer deserializer(iso, msg.serialized.data(), msg.serialized.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(msg.transfers.size()); ++i) {
    Local<ArrayBuffer> ab = ArrayBuffer::New(iso, msg.transfers[i]);
    deserializer.TransferArrayBuffer(i, ab);
  }
  Maybe<bool> hdr = deserializer.ReadHeader(ctx);
  if (hdr.IsNothing() || !hdr.FromJust()) {
    return MaybeLocal<Value>();
  }
  return deserializer.ReadValue(ctx);
}

static void push_worker_error(FvWorkerSlot* slot, const std::string& err_msg) {
  FvWorkerMessage m;
  m.kind = FV_WORKER_MSG_ERROR;
  m.text = err_msg;
  std::lock_guard<std::mutex> lk(slot->mu);
  slot->outbound.push_back(std::move(m));
}

static std::string format_trycatch(Isolate* iso, Local<Context> ctx, TryCatch& tc) {
  if (!tc.HasCaught()) return "Unknown worker error";
  Local<String> s;
  std::string msg = "WorkerError";
  if (tc.Exception()->ToString(ctx).ToLocal(&s)) {
    String::Utf8Value u(iso, s);
    if (*u) msg.assign(*u, static_cast<size_t>(u.length()));
  }
  Local<Message> m = tc.Message();
  if (!m.IsEmpty()) {
    int line = m->GetLineNumber(ctx).FromMaybe(0);
    int col = m->GetStartColumn(ctx).FromMaybe(0);
    msg += " (line " + std::to_string(line) + ":" + std::to_string(col) + ")";
  }
  return msg;
}

// --- Worker-side native callbacks ---

static FvWorkerSlot* slot_from_data(const FunctionCallbackInfo<Value>& args) {
  return static_cast<FvWorkerSlot*>(args.Data().As<External>()->Value());
}

static void worker_native_post(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || slot->closed.load() || slot->stop_requested.load()) return;

  Local<Value> data = args.Length() > 0 ? args[0] : Local<Value>(Undefined(iso));
  Local<Value> transfer = args.Length() > 1 ? args[1] : Local<Value>(Undefined(iso));

  FvWorkerMessage msg;
  std::string err;
  if (!serialize_v8_message(iso, ctx, data, transfer, msg, err)) {
    iso->ThrowException(
        Exception::Error(String::NewFromUtf8(iso, err.c_str()).ToLocalChecked()));
    return;
  }
  std::lock_guard<std::mutex> lk(slot->mu);
  slot->outbound.push_back(std::move(msg));
}

static void worker_native_close(const FunctionCallbackInfo<Value>& args) {
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot) return;
  slot->closed.store(true);
  slot->cv.notify_all();
}

static void worker_native_now(const FunctionCallbackInfo<Value>& args) {
  FvWorkerSlot* slot = slot_from_data(args);
  double elapsed = slot ? (steady_now_ms() - slot->start_ms) : 0.0;
  if (elapsed < 0.0) elapsed = 0.0;
  args.GetReturnValue().Set(Number::New(args.GetIsolate(), elapsed));
}

static void worker_native_tid(const FunctionCallbackInfo<Value>& args) {
  uint64_t tid = get_os_tid();
  std::string s = std::to_string(tid);
  args.GetReturnValue().Set(
      String::NewFromUtf8(args.GetIsolate(), s.c_str()).ToLocalChecked());
}

static void worker_native_log(const FunctionCallbackInfo<Value>& args) {
  if (!std::getenv("FV_WORKER_LOG")) return;
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  std::string line;
  for (int i = 0; i < args.Length(); ++i) {
    if (i > 0) line.push_back(' ');
    Local<String> s;
    if (args[i]->ToString(ctx).ToLocal(&s)) {
      String::Utf8Value u(iso, s);
      if (*u) line.append(*u, static_cast<size_t>(u.length()));
    }
  }
  std::fprintf(stderr, "[fv-worker] %s\n", line.c_str());
}

static void worker_native_set_timer(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || args.Length() < 1 || !args[0]->IsFunction()) {
    args.GetReturnValue().Set(Integer::New(iso, 0));
    return;
  }
  double delay = 0.0;
  if (args.Length() > 1 && !args[1]->IsUndefined()) {
    delay = args[1]->NumberValue(ctx).FromMaybe(0.0);
    if (!(delay > 0.0)) delay = 0.0;
  }
  bool repeat = args.Length() > 2 && args[2]->BooleanValue(iso);
  int32_t id = slot->next_timer_id++;
  FvWorkerTimer t;
  t.id = id;
  t.fire_at_ms = steady_now_ms() + delay;
  t.interval_ms = repeat ? (delay > 1.0 ? delay : 1.0) : 0.0;
  t.repeat = repeat;
  t.cb.Reset(iso, args[0].As<Function>());
  slot->timers.push_back(std::move(t));
  args.GetReturnValue().Set(Integer::New(iso, id));
}

static void worker_native_clear_timer(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || args.Length() < 1) return;
  int32_t id = args[0]->Int32Value(ctx).FromMaybe(0);
  for (auto it = slot->timers.begin(); it != slot->timers.end(); ++it) {
    if (it->id == id) {
      it->cb.Reset();
      slot->timers.erase(it);
      break;
    }
  }
}

static void worker_native_queue_microtask(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  if (args.Length() > 0 && args[0]->IsFunction()) {
    iso->EnqueueMicrotask(args[0].As<Function>());
  }
}

static std::string worker_norm_path(const std::string& raw) {
  std::string s = raw;
  // Strip chrome-extension://<id>/ prefix if present
  const std::string pfx = "chrome-extension://";
  if (s.compare(0, pfx.size(), pfx) == 0) {
    size_t sl = s.find('/', pfx.size());
    s = (sl != std::string::npos) ? s.substr(sl + 1) : "";
  }
  // Strip query and fragment
  size_t q = s.find_first_of("?#");
  if (q != std::string::npos) s.resize(q);
  // Collapse ./ and ../ segments
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') i++;
    size_t j = s.find('/', i);
    if (j == std::string::npos) j = s.size();
    std::string seg = s.substr(i, j - i);
    i = j;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
    } else {
      parts.push_back(std::move(seg));
    }
  }
  std::string out;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k > 0) out.push_back('/');
    out += parts[k];
  }
  return out;
}

static std::string worker_origin_prefix(const std::string& url) {
  size_t s = url.find("://");
  if (s == std::string::npos) return "";
  size_t slash = url.find('/', s + 3);
  if (slash == std::string::npos) return url + "/";
  return url.substr(0, slash + 1);
}

static std::string worker_strip_query_hash(const std::string& p) {
  size_t cut = p.find_first_of("?#");
  return cut == std::string::npos ? p : p.substr(0, cut);
}

static std::string worker_resolve_rel(const std::string& worker_url, const std::string& base,
                                      const std::string& spec, bool& ok_out) {
  ok_out = true;
  std::string origin = worker_origin_prefix(worker_url);
  size_t scheme_pos = spec.find("://");
  if (scheme_pos != std::string::npos) {
    if (!origin.empty() && spec.compare(0, origin.size(), origin) != 0) {
      ok_out = false;
      return "";
    }
    std::string tail = !origin.empty() ? spec.substr(origin.size()) : spec.substr(scheme_pos + 3);
    std::string suffix;
    size_t qh = tail.find_first_of("?#");
    if (qh != std::string::npos) {
      suffix = tail.substr(qh);
      tail = tail.substr(0, qh);
    }
    return worker_norm_path(tail) + suffix;
  }
  std::string suffix;
  std::string raw_path = spec;
  size_t qh = spec.find_first_of("?#");
  if (qh != std::string::npos) {
    suffix = spec.substr(qh);
    raw_path = spec.substr(0, qh);
  }
  if (!raw_path.empty() && raw_path[0] == '/') {
    return worker_norm_path(raw_path) + suffix;
  }
  std::string nb = worker_norm_path(worker_strip_query_hash(base));
  if (raw_path.empty()) {
    // Query-only (`?foo`) or fragment-only (`#bar`) reference: keep referrer filename!
    if (!suffix.empty() && suffix[0] == '#') {
      size_t hpos = base.find('#');
      std::string bnohash = (hpos != std::string::npos) ? base.substr(0, hpos) : base;
      size_t qpos = bnohash.find('?');
      std::string bpath = worker_norm_path(qpos != std::string::npos ? bnohash.substr(0, qpos) : bnohash);
      std::string bquery = (qpos != std::string::npos) ? bnohash.substr(qpos) : "";
      return bpath + bquery + suffix;
    }
    return nb + suffix;
  }
  size_t sl = nb.rfind('/');
  std::string dir = (sl != std::string::npos) ? nb.substr(0, sl + 1) : "";
  return worker_norm_path(dir + raw_path) + suffix;
}

static void worker_native_reg_file(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || args.Length() < 2) return;
  Local<String> sp, sc;
  if (!args[0]->ToString(ctx).ToLocal(&sp) || !args[1]->ToString(ctx).ToLocal(&sc)) return;
  String::Utf8Value up(iso, sp);
  String::Utf8Value uc(iso, sc);
  if (!*up) return;
  std::string key = worker_norm_path(std::string(*up, static_cast<size_t>(up.length())));
  slot->vfs[key] = *uc ? std::string(*uc, static_cast<size_t>(uc.length())) : std::string();
}

static void worker_native_read_file(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || args.Length() < 1) {
    args.GetReturnValue().SetNull();
    return;
  }
  Local<String> sp;
  if (!args[0]->ToString(ctx).ToLocal(&sp)) {
    args.GetReturnValue().SetNull();
    return;
  }
  String::Utf8Value up(iso, sp);
  if (!*up) {
    args.GetReturnValue().SetNull();
    return;
  }
  std::string key = worker_norm_path(std::string(*up, static_cast<size_t>(up.length())));
  auto it = slot->vfs.find(key);
  if (it == slot->vfs.end()) {
    args.GetReturnValue().SetNull();
    return;
  }
  Local<String> out;
  if (String::NewFromUtf8(iso, it->second.data(), NewStringType::kNormal,
                          static_cast<int>(it->second.size()))
          .ToLocal(&out)) {
    args.GetReturnValue().Set(out);
  } else {
    args.GetReturnValue().SetNull();
  }
}

static void worker_native_b64_decode(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  if (args.Length() < 1) {
    args.GetReturnValue().Set(ArrayBuffer::New(iso, 0));
    return;
  }
  Local<String> s;
  if (!args[0]->ToString(ctx).ToLocal(&s)) {
    args.GetReturnValue().Set(ArrayBuffer::New(iso, 0));
    return;
  }
  String::Utf8Value u(iso, s);
  const char* p = *u ? *u : "";
  size_t n = static_cast<size_t>(u.length());
  static const int8_t T[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };
  std::vector<uint8_t> out;
  out.reserve((n * 3) / 4 + 4);
  uint32_t val = 0;
  int valb = -8;
  bool seen_pad = false;
  int pad_count = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char ch = static_cast<unsigned char>(p[i]);
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
    if (ch == '=') {
      seen_pad = true;
      pad_count++;
      if (pad_count > 2) {
        iso->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(iso, "Invalid base64 padding")));
        return;
      }
      continue;
    }
    if (seen_pad) {
      iso->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(iso, "Unexpected character after base64 padding")));
      return;
    }
    int8_t d = T[ch];
    if (d < 0) {
      iso->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(iso, "Invalid base64 character")));
      return;
    }
    val = (val << 6) | static_cast<uint32_t>(d);
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  Local<ArrayBuffer> ab = ArrayBuffer::New(iso, out.size());
  if (!out.empty()) {
    std::memcpy(ab->GetBackingStore()->Data(), out.data(), out.size());
  }
  args.GetReturnValue().Set(ab);
}

static void worker_native_b64_encode(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  if (args.Length() < 1) {
    args.GetReturnValue().Set(String::Empty(iso));
    return;
  }
  const uint8_t* ptr = nullptr;
  size_t len = 0;
  std::shared_ptr<BackingStore> bs;
  if (args[0]->IsArrayBuffer()) {
    bs = args[0].As<ArrayBuffer>()->GetBackingStore();
    ptr = static_cast<const uint8_t*>(bs->Data());
    len = bs->ByteLength();
  } else if (args[0]->IsArrayBufferView()) {
    Local<ArrayBufferView> v = args[0].As<ArrayBufferView>();
    bs = v->Buffer()->GetBackingStore();
    ptr = static_cast<const uint8_t*>(bs->Data()) + v->ByteOffset();
    len = v->ByteLength();
  } else {
    args.GetReturnValue().Set(String::Empty(iso));
    return;
  }
  if (len > (static_cast<size_t>(v8::String::kMaxLength) / 4) * 3) {
    iso->ThrowException(Exception::RangeError(String::NewFromUtf8Literal(iso, "Buffer too large for base64 encoding")));
    return;
  }
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t a = ptr[i];
    uint32_t b = (i + 1 < len) ? ptr[i + 1] : 0;
    uint32_t c = (i + 2 < len) ? ptr[i + 2] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(B64[(triple >> 18) & 0x3F]);
    out.push_back(B64[(triple >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? B64[(triple >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? B64[triple & 0x3F] : '=');
  }
  Local<String> s;
  if (String::NewFromUtf8(iso, out.data(), NewStringType::kNormal, static_cast<int>(out.size()))
          .ToLocal(&s)) {
    args.GetReturnValue().Set(s);
  } else {
    args.GetReturnValue().Set(String::Empty(iso));
  }
}

static thread_local FvWorkerSlot* tls_worker_slot = nullptr;

static std::string worker_find_mod_url(FvWorkerSlot* slot, Isolate* iso, Local<Module> m) {
  for (const auto& kv : slot->mod_cache) {
    if (kv.second && !kv.second->IsEmpty() && kv.second->Get(iso) == m) {
      return kv.first;
    }
  }
  auto it = slot->mod_urls.find(m->GetIdentityHash());
  return it != slot->mod_urls.end() ? it->second : "";
}

static void worker_init_import_meta(Local<Context> ctx, Local<Module> module, Local<Object> meta) {
  Isolate* iso = ctx->GetIsolate();
  FvWorkerSlot* slot = tls_worker_slot;
  if (!slot) return;
  std::string mod_key = worker_find_mod_url(slot, iso, module);
  std::string full_url = worker_origin_prefix(slot->url) + mod_key;
  if (full_url.empty()) full_url = slot->url;
  Local<String> u;
  if (String::NewFromUtf8(iso, full_url.data(), NewStringType::kNormal, static_cast<int>(full_url.size())).ToLocal(&u)) {
    (void)meta->CreateDataProperty(ctx, String::NewFromUtf8Literal(iso, "url"), u).FromMaybe(false);
  }
}

static MaybeLocal<Module> worker_load_module(Isolate* iso, FvWorkerSlot* slot, const std::string& mod_key) {
  auto it = slot->mod_cache.find(mod_key);
  if (it != slot->mod_cache.end() && it->second && !it->second->IsEmpty()) {
    return it->second->Get(iso);
  }
  std::string asset_key = worker_norm_path(worker_strip_query_hash(mod_key));
  auto fit = slot->vfs.find(asset_key);
  if (fit == slot->vfs.end()) {
    std::string msg = "Module not found in extension VFS: " + asset_key;
    iso->ThrowException(Exception::TypeError(String::NewFromUtf8(iso, msg.c_str()).ToLocalChecked()));
    return MaybeLocal<Module>();
  }
  Local<String> src_str;
  if (!String::NewFromUtf8(iso, fit->second.data(), NewStringType::kNormal,
                           static_cast<int>(fit->second.size()))
           .ToLocal(&src_str)) {
    return MaybeLocal<Module>();
  }
  std::string full_url = worker_origin_prefix(slot->url) + mod_key;
  Local<String> url_str = String::NewFromUtf8(iso, full_url.data(), NewStringType::kNormal,
                                              static_cast<int>(full_url.size()))
                              .ToLocalChecked();
  ScriptOrigin origin(url_str, 0, 0, false, -1, Local<Value>(), false, false, true);
  ScriptCompiler::Source src(src_str, origin);
  Local<Module> m;
  if (!ScriptCompiler::CompileModule(iso, &src).ToLocal(&m)) {
    return MaybeLocal<Module>();
  }
  slot->mod_cache[mod_key] = new Global<Module>(iso, m);
  slot->mod_urls[m->GetIdentityHash()] = mod_key;
  return m;
}

static MaybeLocal<Module> worker_resolve_module(Local<Context> ctx, Local<String> spec,
                                                Local<FixedArray>, Local<Module> referrer) {
  Isolate* iso = ctx->GetIsolate();
  FvWorkerSlot* slot = tls_worker_slot;
  if (!slot) return MaybeLocal<Module>();
  String::Utf8Value us(iso, spec);
  std::string spec_s = *us ? std::string(*us, static_cast<size_t>(us.length())) : "";
  std::string base = worker_find_mod_url(slot, iso, referrer);
  bool ok = true;
  std::string resolved = worker_resolve_rel(slot->url, base, spec_s, ok);
  if (!ok) {
    std::string msg = "Cross-origin module import blocked: " + spec_s;
    iso->ThrowException(Exception::TypeError(String::NewFromUtf8(iso, msg.c_str()).ToLocalChecked()));
    return MaybeLocal<Module>();
  }
  return worker_load_module(iso, slot, resolved);
}

static void worker_dyn_import_fulfilled(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  Local<Array> pair = args.Data().As<Array>();
  Local<Value> r_val, ns_val;
  if (pair->Get(ctx, 0).ToLocal(&r_val) && pair->Get(ctx, 1).ToLocal(&ns_val)) {
    Local<Promise::Resolver> r = r_val.As<Promise::Resolver>();
    (void)r->Resolve(ctx, ns_val).FromMaybe(false);
  }
}

static void worker_dyn_import_rejected(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  Local<Promise::Resolver> r = args.Data().As<Promise::Resolver>();
  Local<Value> reason = args.Length() > 0 ? args[0] : Local<Value>(Undefined(iso));
  (void)r->Reject(ctx, reason).FromMaybe(false);
}

static MaybeLocal<Promise> worker_import_dynamic(Local<Context> ctx, Local<Data>,
                                                 Local<Value> resource, Local<String> spec,
                                                 Local<FixedArray>) {
  Isolate* iso = ctx->GetIsolate();
  Local<Promise::Resolver> r = Promise::Resolver::New(ctx).ToLocalChecked();
  FvWorkerSlot* slot = tls_worker_slot;
  if (!slot) {
    r->Reject(ctx, Exception::Error(String::NewFromUtf8Literal(iso, "no worker context"))).Check();
    return r->GetPromise();
  }
  std::string base = "";
  if (resource->IsString()) {
    String::Utf8Value ub(iso, resource);
    if (*ub) base.assign(*ub, static_cast<size_t>(ub.length()));
  }
  String::Utf8Value us(iso, spec);
  std::string spec_s = *us ? std::string(*us, static_cast<size_t>(us.length())) : "";
  bool ok = true;
  std::string resolved = worker_resolve_rel(slot->url, base, spec_s, ok);
  if (!ok) {
    std::string msg = "Cross-origin dynamic import blocked: " + spec_s;
    r->Reject(ctx, Exception::TypeError(String::NewFromUtf8(iso, msg.c_str()).ToLocalChecked())).Check();
    return r->GetPromise();
  }
  TryCatch tc(iso);
  Local<Module> m;
  if (!worker_load_module(iso, slot, resolved).ToLocal(&m)) {
    r->Reject(ctx, tc.HasCaught() ? tc.Exception() : Local<Value>(String::NewFromUtf8Literal(iso, "import failed"))).Check();
    return r->GetPromise();
  }
  if (m->GetStatus() == Module::kUninstantiated &&
      !m->InstantiateModule(ctx, worker_resolve_module).FromMaybe(false)) {
    r->Reject(ctx, tc.HasCaught() ? tc.Exception() : Local<Value>(String::NewFromUtf8Literal(iso, "instantiate failed"))).Check();
    return r->GetPromise();
  }
  Local<Value> ev;
  if (!m->Evaluate(ctx).ToLocal(&ev)) {
    r->Reject(ctx, tc.HasCaught() ? tc.Exception() : Local<Value>(String::NewFromUtf8Literal(iso, "evaluate failed"))).Check();
    return r->GetPromise();
  }
  if (ev->IsPromise()) {
    Local<Promise> ep = ev.As<Promise>();
    if (ep->State() == Promise::kRejected) {
      r->Reject(ctx, ep->Result()).Check();
    } else if (ep->State() == Promise::kFulfilled) {
      r->Resolve(ctx, m->GetModuleNamespace()).Check();
    } else {
      Local<Array> pair = Array::New(iso, 2);
      pair->Set(ctx, 0, r).Check();
      pair->Set(ctx, 1, m->GetModuleNamespace()).Check();
      Local<Function> on_ok, on_err;
      if (Function::New(ctx, worker_dyn_import_fulfilled, pair).ToLocal(&on_ok) &&
          Function::New(ctx, worker_dyn_import_rejected, r).ToLocal(&on_err)) {
        Local<Promise> chained;
        if (!ep->Then(ctx, on_ok, on_err).ToLocal(&chained)) {
          (void)r->Resolve(ctx, m->GetModuleNamespace()).FromMaybe(false);
        }
      } else {
        r->Resolve(ctx, m->GetModuleNamespace()).Check();
      }
    }
  } else {
    r->Resolve(ctx, m->GetModuleNamespace()).Check();
  }
  return r->GetPromise();
}

static void worker_native_run_module(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  FvWorkerSlot* slot = slot_from_data(args);
  if (!slot || args.Length() < 1) return;
  Local<String> sp;
  if (!args[0]->ToString(ctx).ToLocal(&sp)) return;
  String::Utf8Value up(iso, sp);
  if (!*up) return;
  std::string key = worker_norm_path(std::string(*up, static_cast<size_t>(up.length())));
  Local<Module> m;
  if (!worker_load_module(iso, slot, key).ToLocal(&m)) return;
  if (m->GetStatus() == Module::kUninstantiated &&
      !m->InstantiateModule(ctx, worker_resolve_module).FromMaybe(false)) {
    return;
  }
  Local<Value> ev;
  if (!m->Evaluate(ctx).ToLocal(&ev)) return;
  if (ev->IsPromise() && ev.As<Promise>()->State() == Promise::kRejected) {
    iso->ThrowException(ev.As<Promise>()->Result());
    return;
  }
  args.GetReturnValue().Set(ev);
}

// DedicatedWorkerGlobalScope JS bootstrap (self-contained inside each worker Isolate).
static const char* kWorkerBootstrapJS = R"JS(
(function(global) {
  "use strict";
  global.self = global;
  global.globalThis = global;
  global.__fvWorkerThreadId = __fvWTid();

  class Event {
    constructor(type, init = {}) {
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.defaultPrevented = false;
      this.timeStamp = __fvWNow();
      this.target = null;
      this.currentTarget = null;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() {}
    stopImmediatePropagation() {}
  }
  class MessageEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.data = init.data !== undefined ? init.data : null;
      this.origin = init.origin || "";
      this.lastEventId = init.lastEventId || "";
      this.source = init.source || null;
      this.ports = Array.isArray(init.ports) ? init.ports : [];
    }
  }
  class ErrorEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.message = init.message || "";
      this.filename = init.filename || "";
      this.lineno = init.lineno || 0;
      this.colno = init.colno || 0;
      this.error = init.error !== undefined ? init.error : null;
    }
  }
  class EventTarget {
    constructor() { this._l = Object.create(null); }
    addEventListener(type, fn, opts) {
      if (typeof fn !== "function") return;
      const t = String(type);
      const list = this._l[t] || (this._l[t] = []);
      for (let i = 0; i < list.length; i++) if (list[i].fn === fn) return;
      list.push({ fn, once: !!(opts && opts.once) });
    }
    removeEventListener(type, fn) {
      const list = this._l[String(type)];
      if (!list) return;
      for (let i = 0; i < list.length; i++) {
        if (list[i].fn === fn) { list.splice(i, 1); return; }
      }
    }
    dispatchEvent(ev) {
      ev.target = this;
      ev.currentTarget = this;
      const on = this["on" + ev.type];
      if (typeof on === "function") on.call(this, ev);
      const list = this._l[ev.type];
      if (list && list.length) {
        const snap = list.slice();
        for (let i = 0; i < snap.length; i++) {
          if (snap[i].once) this.removeEventListener(ev.type, snap[i].fn);
          snap[i].fn.call(this, ev);
        }
      }
      return !ev.defaultPrevented;
    }
  }

  const _rootTarget = new EventTarget();
  global.Event = Event;
  global.MessageEvent = MessageEvent;
  global.ErrorEvent = ErrorEvent;
  global.EventTarget = EventTarget;
  global.WorkerGlobalScope = class WorkerGlobalScope extends EventTarget {};
  global.DedicatedWorkerGlobalScope = class DedicatedWorkerGlobalScope extends global.WorkerGlobalScope {};
  global.onmessage = null;
  global.onerror = null;

  global.addEventListener = (t, fn, o) => _rootTarget.addEventListener(t, fn, o);
  global.removeEventListener = (t, fn) => _rootTarget.removeEventListener(t, fn);
  global.dispatchEvent = (ev) => {
    ev.target = global;
    ev.currentTarget = global;
    const on = global["on" + ev.type];
    if (typeof on === "function") on.call(global, ev);
    const list = _rootTarget._l[ev.type];
    if (list && list.length) {
      const snap = list.slice();
      for (let i = 0; i < snap.length; i++) {
        if (snap[i].once) _rootTarget.removeEventListener(ev.type, snap[i].fn);
        snap[i].fn.call(global, ev);
      }
    }
    return !ev.defaultPrevented;
  };

  global.postMessage = (data, transfer) => __fvWPost(data, transfer);
  global.close = () => __fvWClose();
  global.setTimeout = (fn, ms, ...args) => {
    if (typeof fn !== "function") return 0;
    const cb = args.length ? () => fn(...args) : fn;
    return __fvWSetTimer(cb, ms || 0, false);
  };
  global.clearTimeout = (id) => __fvWClearTimer(id | 0);
  global.setInterval = (fn, ms, ...args) => {
    if (typeof fn !== "function") return 0;
    const cb = args.length ? () => fn(...args) : fn;
    return __fvWSetTimer(cb, ms || 0, true);
  };
  global.clearInterval = (id) => __fvWClearTimer(id | 0);
  global.queueMicrotask = (fn) => __fvWQueueMicrotask(fn);

  global.console = {
    log: (...a) => __fvWLog("log", ...a),
    info: (...a) => __fvWLog("info", ...a),
    warn: (...a) => __fvWLog("warn", ...a),
    error: (...a) => __fvWLog("error", ...a),
    debug: (...a) => __fvWLog("debug", ...a),
  };
  global.performance = {
    timeOrigin: Date.now() - __fvWNow(),
    now: () => __fvWNow(),
  };

  // atob / btoa
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  global.btoa = function(str) {
    str = String(str);
    let out = "";
    for (let i = 0; i < str.length; i += 3) {
      const a = str.charCodeAt(i);
      const b = i + 1 < str.length ? str.charCodeAt(i + 1) : NaN;
      const c = i + 2 < str.length ? str.charCodeAt(i + 2) : NaN;
      if (a > 255 || b > 255 || c > 255) throw new Error("InvalidCharacterError");
      out += B64[a >> 2];
      out += B64[((a & 3) << 4) | (isNaN(b) ? 0 : (b >> 4))];
      out += isNaN(b) ? "=" : B64[((b & 15) << 2) | (isNaN(c) ? 0 : (c >> 6))];
      out += isNaN(c) ? "=" : B64[c & 63];
    }
    return out;
  };
  global.atob = function(str) {
    str = String(str).replace(/[\t\n\f\r ]+/g, "");
    if (str.length % 4 === 1) throw new Error("InvalidCharacterError");
    let out = "";
    for (let i = 0; i < str.length; i += 4) {
      const a = B64.indexOf(str[i]);
      const b = B64.indexOf(str[i + 1]);
      const c = str[i + 2] === "=" ? -1 : B64.indexOf(str[i + 2]);
      const d = str[i + 3] === "=" ? -1 : B64.indexOf(str[i + 3]);
      if (a < 0 || b < 0 || (str[i + 2] !== "=" && c < 0) || (str[i + 3] !== "=" && d < 0))
        throw new Error("InvalidCharacterError");
      out += String.fromCharCode((a << 2) | (b >> 4));
      if (c >= 0) out += String.fromCharCode(((b & 15) << 4) | (c >> 2));
      if (d >= 0) out += String.fromCharCode(((c & 3) << 6) | d);
    }
    return out;
  };

  // TextEncoder / TextDecoder (UTF-8)
  global.TextEncoder = class TextEncoder {
    get encoding() { return "utf-8"; }
    encode(str = "") {
      str = String(str);
      const bytes = [];
      for (let i = 0; i < str.length; i++) {
        let cp = str.charCodeAt(i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < str.length) {
          const lo = str.charCodeAt(i + 1);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
          }
        }
        if (cp < 0x80) bytes.push(cp);
        else if (cp < 0x800) bytes.push(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F));
        else if (cp < 0x10000) bytes.push(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
        else bytes.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
      }
      return new Uint8Array(bytes);
    }
  };
  global.TextDecoder = class TextDecoder {
    constructor(label = "utf-8") { this.encoding = String(label).toLowerCase(); }
    decode(input) {
      if (!input) return "";
      const bytes = input instanceof Uint8Array
        ? input
        : ArrayBuffer.isView(input)
          ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength)
          : new Uint8Array(input);
      let out = "";
      for (let i = 0; i < bytes.length; ) {
        const b0 = bytes[i++];
        if (b0 < 0x80) { out += String.fromCharCode(b0); }
        else if ((b0 & 0xE0) === 0xC0 && i < bytes.length) {
          out += String.fromCharCode(((b0 & 0x1F) << 6) | (bytes[i++] & 0x3F));
        } else if ((b0 & 0xF0) === 0xE0 && i + 1 < bytes.length) {
          out += String.fromCharCode(((b0 & 0x0F) << 12) | ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F));
        } else if ((b0 & 0xF8) === 0xF0 && i + 2 < bytes.length) {
          let cp = ((b0 & 0x07) << 18) | ((bytes[i++] & 0x3F) << 12) | ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F);
          cp -= 0x10000;
          out += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
        } else {
          out += "\uFFFD";
        }
      }
      return out;
    }
  };

  global.navigator = { hardwareConcurrency: 4, userAgent: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36", language: "en-US", languages: ["en-US", "en"] };
  global.location = { href: global.__fvWorkerUrl || "about:blank" };

  class DOMException extends Error {
    constructor(message = "", name = "Error") {
      super(String(message));
      this.name = String(name);
    }
  }
  global.DOMException = DOMException;

  class URLSearchParams {
    constructor(init = "") {
      this._entries = [];
      if (typeof init === "string") {
        const s = init.startsWith("?") ? init.slice(1) : init;
        if (s) {
          for (const part of s.split("&")) {
            const idx = part.indexOf("=");
            const k = decodeURIComponent((idx >= 0 ? part.slice(0, idx) : part).replace(/\+/g, " "));
            const v = idx >= 0 ? decodeURIComponent(part.slice(idx + 1).replace(/\+/g, " ")) : "";
            this._entries.push([k, v]);
          }
        }
      } else if (init instanceof URLSearchParams) {
        for (const [k, v] of init._entries) this._entries.push([k, v]);
      } else if (Array.isArray(init)) {
        for (const pair of init) this._entries.push([String(pair[0]), String(pair[1])]);
      } else if (init && typeof init === "object") {
        for (const k of Object.keys(init)) this._entries.push([k, String(init[k])]);
      }
    }
    append(k, v) { this._entries.push([String(k), String(v)]); }
    delete(k) { const ks = String(k); this._entries = this._entries.filter(e => e[0] !== ks); }
    get(k) { const ks = String(k); const f = this._entries.find(e => e[0] === ks); return f ? f[1] : null; }
    getAll(k) { const ks = String(k); return this._entries.filter(e => e[0] === ks).map(e => e[1]); }
    has(k) { const ks = String(k); return this._entries.some(e => e[0] === ks); }
    set(k, v) {
      const ks = String(k), vs = String(v);
      let set = false;
      this._entries = this._entries.filter(e => {
        if (e[0] !== ks) return true;
        if (!set) { e[1] = vs; set = true; return true; }
        return false;
      });
      if (!set) this._entries.push([ks, vs]);
    }
    forEach(cb, thisArg) { for (const [k, v] of this._entries) cb.call(thisArg, v, k, this); }
    entries() { return this._entries[Symbol.iterator](); }
    keys() { return this._entries.map(e => e[0])[Symbol.iterator](); }
    values() { return this._entries.map(e => e[1])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() {
      return this._entries.map(([k, v]) => encodeURIComponent(k).replace(/%20/g, "+") + "=" + encodeURIComponent(v).replace(/%20/g, "+")).join("&");
    }
  }
  global.URLSearchParams = URLSearchParams;

  class URL {
    constructor(input, base) {
      let s = String(input);
      if (base !== undefined && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(s)) {
        const b = base instanceof URL ? base : new URL(String(base));
        if (s.startsWith("//")) s = b.protocol + s;
        else if (s.startsWith("/")) s = b.origin + s;
        else if (s.startsWith("?")) s = b.origin + b.pathname + s;
        else if (s.startsWith("#")) s = b.origin + b.pathname + b.search + s;
        else {
          const dir = b.pathname.slice(0, b.pathname.lastIndexOf("/") + 1);
          const segs = (dir + s).split("/");
          const stack = [];
          for (const seg of segs) {
            if (!seg || seg === ".") continue;
            if (seg === "..") stack.pop();
            else stack.push(seg);
          }
          s = b.origin + "/" + stack.join("/");
        }
      }
      const m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(s);
      if (!m) throw new TypeError("Invalid URL: " + input);
      this.protocol = m[1].toLowerCase();
      const auth = m[2] || "";
      const at = auth.lastIndexOf("@");
      const hostport = at >= 0 ? auth.slice(at + 1) : auth;
      const col = hostport.lastIndexOf(":");
      this.host = hostport;
      this.hostname = col >= 0 && !hostport.endsWith("]") ? hostport.slice(0, col) : hostport;
      this.port = col >= 0 && !hostport.endsWith("]") ? hostport.slice(col + 1) : "";
      this.pathname = m[3] || "/";
      this.search = m[4] || "";
      this.hash = m[5] || "";
      this.origin = this.protocol + "//" + this.host;
      this.searchParams = new URLSearchParams(this.search);
    }
    get href() {
      const qs = this.searchParams.toString();
      const searchStr = qs ? "?" + qs : (this.search || "");
      return this.protocol + "//" + this.host + this.pathname + searchStr + this.hash;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
  }
  global.URL = URL;

  class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) { const s = new AbortSignal(); s.aborted = true; s.reason = reason !== undefined ? reason : new DOMException("Aborted", "AbortError"); return s; }
  }
  class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
      if (this.signal.aborted) return;
      this.signal.aborted = true;
      this.signal.reason = reason !== undefined ? reason : new DOMException("Aborted", "AbortError");
      this.signal.dispatchEvent(new Event("abort"));
    }
  }
  global.AbortSignal = AbortSignal;
  global.AbortController = AbortController;

  class Blob {
    constructor(parts = [], opts = {}) {
      const chunks = [];
      const enc = new TextEncoder();
      let total = 0;
      for (const p of parts) {
        let u8;
        if (p instanceof Blob) u8 = p._bytes;
        else if (p instanceof ArrayBuffer) u8 = new Uint8Array(p.slice(0));
        else if (ArrayBuffer.isView(p)) u8 = new Uint8Array(p.buffer.slice(p.byteOffset, p.byteOffset + p.byteLength));
        else u8 = enc.encode(String(p));
        chunks.push(u8);
        total += u8.byteLength;
      }
      const out = new Uint8Array(total);
      let off = 0;
      for (const c of chunks) { out.set(c, off); off += c.byteLength; }
      this._bytes = out;
      this.size = total;
      this.type = opts && opts.type ? String(opts.type).toLowerCase() : "";
    }
    async arrayBuffer() { return this._bytes.buffer.slice(0); }
    async bytes() { return new Uint8Array(this._bytes); }
    async text() { return new TextDecoder().decode(this._bytes); }
    slice(start = 0, end = this.size, type = "") {
      const b = new Blob([], { type });
      b._bytes = this._bytes.slice(start, end);
      b.size = b._bytes.byteLength;
      return b;
    }
  }
  global.Blob = Blob;

  class Headers {
    constructor(init) {
      this._map = new Map();
      if (init instanceof Headers) {
        init.forEach((v, k) => this.append(k, v));
      } else if (Array.isArray(init)) {
        for (const [k, v] of init) this.append(k, v);
      } else if (init && typeof init === "object") {
        for (const k of Object.keys(init)) this.append(k, init[k]);
      }
    }
    append(k, v) {
      const lk = String(k).toLowerCase();
      const prev = this._map.get(lk);
      this._map.set(lk, prev !== undefined ? prev + ", " + String(v) : String(v));
    }
    delete(k) { this._map.delete(String(k).toLowerCase()); }
    get(k) { const v = this._map.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    has(k) { return this._map.has(String(k).toLowerCase()); }
    set(k, v) { this._map.set(String(k).toLowerCase(), String(v)); }
    forEach(cb, thisArg) { for (const [k, v] of this._map.entries()) cb.call(thisArg, v, k, this); }
    entries() { return this._map.entries(); }
    keys() { return this._map.keys(); }
    values() { return this._map.values(); }
    [Symbol.iterator]() { return this._map.entries(); }
  }
  global.Headers = Headers;

  class ReadableStream {
    constructor(underlying = {}) {
      this._chunks = [];
      this._closed = false;
      const ctrl = {
        enqueue: (c) => { this._chunks.push(c); },
        close: () => { this._closed = true; },
        error: () => { this._closed = true; },
      };
      if (typeof underlying.start === "function") underlying.start(ctrl);
    }
    getReader() {
      let i = 0;
      const chunks = this._chunks;
      return {
        read() {
          if (i < chunks.length) return Promise.resolve({ value: chunks[i++], done: false });
          return Promise.resolve({ value: undefined, done: true });
        },
        releaseLock() {},
        cancel() { return Promise.resolve(); },
      };
    }
  }
  global.ReadableStream = ReadableStream;

  class Request {
    constructor(input, init = {}) {
      if (input instanceof Request) {
        this.url = input.url;
        this.method = (init.method || input.method || "GET").toUpperCase();
        this.headers = new Headers(init.headers || input.headers);
        this.body = init.body !== undefined ? init.body : input.body;
      } else {
        this.url = String(input);
        this.method = (init.method || "GET").toUpperCase();
        this.headers = new Headers(init.headers);
        this.body = init.body !== undefined ? init.body : null;
      }
      this.credentials = init.credentials || "same-origin";
      this.signal = init.signal || null;
    }
  }
  global.Request = Request;

  class Response {
    constructor(body = null, init = {}) {
      this.status = init.status !== undefined ? Number(init.status) : 200;
      this.ok = this.status >= 200 && this.status < 300;
      this.statusText = init.statusText !== undefined ? String(init.statusText) : (this.ok ? "OK" : "");
      this.headers = new Headers(init.headers);
      this.url = init.url || "";
      this.type = "basic";
      this.redirected = false;
      this.bodyUsed = false;
      if (body == null) this._bytes = new Uint8Array(0);
      else if (body instanceof ArrayBuffer) this._bytes = new Uint8Array(body.slice(0));
      else if (ArrayBuffer.isView(body)) this._bytes = new Uint8Array(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength));
      else if (body instanceof Blob) this._bytes = new Uint8Array(body._bytes);
      else this._bytes = new TextEncoder().encode(String(body));
    }
    get body() {
      const b = this._bytes;
      return new ReadableStream({ start(c) { if (b.byteLength) c.enqueue(b); c.close(); } });
    }
    clone() {
      return new Response(new Uint8Array(this._bytes), { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url });
    }
    async arrayBuffer() { this.bodyUsed = true; return this._bytes.buffer.slice(0); }
    async bytes() { this.bodyUsed = true; return new Uint8Array(this._bytes); }
    async text() { this.bodyUsed = true; return new TextDecoder().decode(this._bytes); }
    async json() { return JSON.parse(await this.text()); }
    async blob() { this.bodyUsed = true; return new Blob([this._bytes], { type: this.headers.get("content-type") || "" }); }
  }
  global.Response = Response;

  class MessagePort extends EventTarget {
    constructor() { super(); this._other = null; this.onmessage = null; }
    postMessage(data) {
      const other = this._other;
      if (!other) return;
      queueMicrotask(() => other.dispatchEvent(new MessageEvent("message", { data })));
    }
    start() {}
    close() { this._other = null; }
  }
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }
  global.MessagePort = MessagePort;
  global.MessageChannel = MessageChannel;

  global.structuredClone = function(v) {
    if (v === null || typeof v !== "object") return v;
    if (v instanceof ArrayBuffer) return v.slice(0);
    if (ArrayBuffer.isView(v)) return new v.constructor(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));
    if (v instanceof Date) return new Date(v.getTime());
    if (v instanceof RegExp) return new RegExp(v.source, v.flags);
    if (v instanceof Map) { const m = new Map(); v.forEach((val, k) => m.set(global.structuredClone(k), global.structuredClone(val))); return m; }
    if (v instanceof Set) { const s = new Set(); v.forEach(val => s.add(global.structuredClone(val))); return s; }
    if (Array.isArray(v)) return v.map(x => global.structuredClone(x));
    const o = {};
    for (const k of Object.keys(v)) o[k] = global.structuredClone(v[k]);
    return o;
  };

  // WebAssembly streaming helpers backed by native V8 WebAssembly
  if (typeof global.WebAssembly === "object" && global.WebAssembly) {
    global.WebAssembly.compileStreaming = async function(respPromise) {
      const resp = await respPromise;
      const buf = await resp.arrayBuffer();
      return global.WebAssembly.compile(buf);
    };
    global.WebAssembly.instantiateStreaming = async function(respPromise, imports) {
      const resp = await respPromise;
      const buf = await resp.arrayBuffer();
      return global.WebAssembly.instantiate(buf, imports);
    };
  }

  // Native OpenSSL/EVP WebCrypto (crypto.getRandomValues, randomUUID, crypto.subtle)
  if (typeof __fvCrypto === "function") {
    const C = __fvCrypto;
    const u8 = v => v instanceof ArrayBuffer ? new Uint8Array(v.slice(0)) : ArrayBuffer.isView(v) ? new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)) : new Uint8Array(0);
    const native = (...a) => { const r = C(...a); if (r === undefined) throw new DOMException(a[0] + " failed", "OperationError"); return r; };
    const normHash = h => {
      const s = typeof h === "string" ? h : (h && h.name ? h.name : "SHA-256");
      const u = String(s).toUpperCase();
      if (u === "SHA-1" || u === "SHA-256" || u === "SHA-384" || u === "SHA-512") return u;
      throw new DOMException("Unsupported hash: " + s, "NotSupportedError");
    };
    const keyStore = new WeakMap();
    class CryptoKey {
      constructor() { throw new TypeError("Illegal constructor"); }
      get type() { return keyStore.get(this).type; }
      get extractable() { return keyStore.get(this).extractable; }
      get algorithm() { return keyStore.get(this).algorithm; }
      get usages() { return keyStore.get(this).usages; }
    }
    const mkKey = (type, extractable, algorithm, usages, data) => {
      const k = Object.create(CryptoKey.prototype);
      keyStore.set(k, { type, extractable, algorithm, usages: [...usages], data });
      return k;
    };
    const subtle = {
      async digest(alg, data) {
        return native("digest", normHash(alg), u8(data));
      },
      async generateKey(alg, extractable, usages) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const name = String(a.name).toUpperCase();
        if (name.startsWith("AES-")) {
          const len = a.length || 256;
          const raw = new Uint8Array(len / 8);
          C("random", raw);
          return mkKey("secret", !!extractable, { name: a.name, length: len }, usages, raw);
        }
        if (name === "HMAC") {
          const h = normHash(a.hash);
          const bits = a.length || (h === "SHA-384" || h === "SHA-512" ? 1024 : 512);
          const raw = new Uint8Array(Math.ceil(bits / 8));
          C("random", raw);
          return mkKey("secret", !!extractable, { name: "HMAC", length: bits, hash: { name: h } }, usages, raw);
        }
        if (name === "ECDSA" || name === "ECDH") {
          const [spki, pkcs8] = native("keygen", "EC", a.namedCurve);
          const ka = { name: a.name, namedCurve: a.namedCurve };
          return {
            publicKey: mkKey("public", true, ka, usages.filter(u => u === "verify"), new Uint8Array(spki)),
            privateKey: mkKey("private", !!extractable, ka, usages.filter(u => u === "sign" || u === "deriveKey" || u === "deriveBits"), new Uint8Array(pkcs8)),
          };
        }
        if (name === "ED25519" || name === "X25519") {
          const [spki, pkcs8] = native("keygen", a.name);
          const ka = { name: a.name };
          return {
            publicKey: mkKey("public", true, ka, usages.filter(u => u === "verify"), new Uint8Array(spki)),
            privateKey: mkKey("private", !!extractable, ka, usages.filter(u => u === "sign" || u === "deriveKey" || u === "deriveBits"), new Uint8Array(pkcs8)),
          };
        }
        if (name.startsWith("RSA")) {
          const [spki, pkcs8] = native("keygen", "RSA", a.modulusLength || 2048, a.publicExponent || new Uint8Array([1, 0, 1]));
          const ka = { name: a.name, modulusLength: a.modulusLength || 2048, publicExponent: a.publicExponent || new Uint8Array([1, 0, 1]), hash: { name: normHash(a.hash) } };
          return {
            publicKey: mkKey("public", true, ka, usages.filter(u => u === "verify" || u === "encrypt" || u === "wrapKey"), new Uint8Array(spki)),
            privateKey: mkKey("private", !!extractable, ka, usages.filter(u => u === "sign" || u === "decrypt" || u === "unwrapKey"), new Uint8Array(pkcs8)),
          };
        }
        throw new DOMException("Unsupported generateKey: " + a.name, "NotSupportedError");
      },
      async importKey(format, keyData, alg, extractable, usages) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const name = String(a.name).toUpperCase();
        const raw = format === "jwk" ? null : u8(keyData);
        if (name.startsWith("AES-")) {
          const k = format === "raw" ? raw : new Uint8Array(__fvWB64Decode(String(keyData.k).replace(/-/g, "+").replace(/_/g, "/")));
          return mkKey("secret", !!extractable, { name: a.name, length: k.length * 8 }, usages, k);
        }
        if (name === "HMAC") {
          const k = format === "raw" ? raw : new Uint8Array(__fvWB64Decode(String(keyData.k).replace(/-/g, "+").replace(/_/g, "/")));
          const h = normHash(a.hash);
          return mkKey("secret", !!extractable, { name: "HMAC", length: a.length || k.length * 8, hash: { name: h } }, usages, k);
        }
        if (name === "PBKDF2" || name === "HKDF") {
          return mkKey("secret", false, { name }, usages, raw);
        }
        if (format === "spki" || format === "pkcs8") {
          const isPriv = format === "pkcs8";
          const ka = a.hash ? { name: a.name, hash: { name: normHash(a.hash) }, namedCurve: a.namedCurve } : { name: a.name, namedCurve: a.namedCurve };
          return mkKey(isPriv ? "private" : "public", !!extractable, ka, usages, raw);
        }
        throw new DOMException("Unsupported importKey: " + format + "/" + a.name, "NotSupportedError");
      },
      async exportKey(format, key) {
        const kd = keyStore.get(key);
        if (!kd || !kd.extractable) throw new DOMException("Key not extractable", "InvalidAccessError");
        if (format === "raw" || format === "spki" || format === "pkcs8") {
          return kd.data.buffer.slice(kd.data.byteOffset, kd.data.byteOffset + kd.data.byteLength);
        }
        if (format === "jwk" && kd.type === "secret") {
          const k = __fvWB64Encode(kd.data).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
          return { kty: "oct", k, ext: kd.extractable, key_ops: kd.usages };
        }
        throw new DOMException("Unsupported exportKey format: " + format, "NotSupportedError");
      },
      async encrypt(alg, key, data) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const kd = keyStore.get(key);
        const name = String(a.name).toUpperCase();
        if (name === "AES-GCM") return native("aes", "GCM", true, kd.data, u8(a.iv), u8(data), a.additionalData ? u8(a.additionalData) : new Uint8Array(0), (a.tagLength || 128) / 8);
        if (name === "AES-CBC") return native("aes", "CBC", true, kd.data, u8(a.iv), u8(data), new Uint8Array(0), 0);
        if (name === "AES-CTR") return native("aes", "CTR", true, kd.data, u8(a.counter), u8(data), new Uint8Array(0), 0);
        if (name === "RSA-OAEP") return native("oaep", true, kd.data, kd.algorithm.hash.name, u8(data), a.label ? u8(a.label) : new Uint8Array(0));
        throw new DOMException("Unsupported encrypt: " + a.name, "NotSupportedError");
      },
      async decrypt(alg, key, data) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const kd = keyStore.get(key);
        const name = String(a.name).toUpperCase();
        if (name === "AES-GCM") return native("aes", "GCM", false, kd.data, u8(a.iv), u8(data), a.additionalData ? u8(a.additionalData) : new Uint8Array(0), (a.tagLength || 128) / 8);
        if (name === "AES-CBC") return native("aes", "CBC", false, kd.data, u8(a.iv), u8(data), new Uint8Array(0), 0);
        if (name === "AES-CTR") return native("aes", "CTR", false, kd.data, u8(a.counter), u8(data), new Uint8Array(0), 0);
        if (name === "RSA-OAEP") return native("oaep", false, kd.data, kd.algorithm.hash.name, u8(data), a.label ? u8(a.label) : new Uint8Array(0));
        throw new DOMException("Unsupported decrypt: " + a.name, "NotSupportedError");
      },
      async sign(alg, key, data) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const kd = keyStore.get(key);
        const name = String(a.name).toUpperCase();
        if (name === "HMAC") return native("hmac", kd.algorithm.hash.name, kd.data, u8(data));
        if (name === "ED25519") return native("sign", "Ed25519", "", kd.data, u8(data), 0);
        if (name === "ECDSA") return native("sign", "ECDSA", normHash(a.hash), kd.data, u8(data), 0);
        if (name === "RSASSA-PKCS1-V1_5" || name === "RSA-PSS") return native("sign", a.name, kd.algorithm.hash.name, kd.data, u8(data), a.saltLength || 0);
        throw new DOMException("Unsupported sign: " + a.name, "NotSupportedError");
      },
      async verify(alg, key, sig, data) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const kd = keyStore.get(key);
        const name = String(a.name).toUpperCase();
        if (name === "HMAC") {
          const mac = new Uint8Array(native("hmac", kd.algorithm.hash.name, kd.data, u8(data)));
          const s = u8(sig);
          if (mac.length !== s.length) return false;
          let diff = 0;
          for (let i = 0; i < mac.length; i++) diff |= mac[i] ^ s[i];
          return diff === 0;
        }
        if (name === "ED25519") return !!C("verify", "Ed25519", "", kd.data, u8(sig), u8(data), 0);
        if (name === "ECDSA") return !!C("verify", "ECDSA", normHash(a.hash), kd.data, u8(sig), u8(data), 0);
        if (name === "RSASSA-PKCS1-V1_5" || name === "RSA-PSS") return !!C("verify", a.name, kd.algorithm.hash.name, kd.data, u8(sig), u8(data), a.saltLength || 0);
        throw new DOMException("Unsupported verify: " + a.name, "NotSupportedError");
      },
      async deriveBits(alg, baseKey, length) {
        const a = typeof alg === "string" ? { name: alg } : alg;
        const kd = keyStore.get(baseKey);
        const name = String(a.name).toUpperCase();
        if (name === "PBKDF2") return native("pbkdf2", normHash(a.hash), kd.data, u8(a.salt), a.iterations, length / 8);
        if (name === "HKDF") return native("hkdf", normHash(a.hash), kd.data, u8(a.salt), u8(a.info), length / 8);
        if (name === "ECDH" || name === "X25519") {
          const peer = keyStore.get(a.public);
          const full = new Uint8Array(native("derive", kd.data, peer.data));
          return length ? full.slice(0, length / 8).buffer : full.buffer;
        }
        throw new DOMException("Unsupported deriveBits: " + a.name, "NotSupportedError");
      },
      async deriveKey(alg, baseKey, derivedAlg, extractable, usages) {
        const da = typeof derivedAlg === "string" ? { name: derivedAlg } : derivedAlg;
        const len = da.length || 256;
        const bits = await subtle.deriveBits(alg, baseKey, len);
        return subtle.importKey("raw", bits, da, extractable, usages);
      },
      async wrapKey(format, key, wrappingKey, wrapAlg) {
        const exported = await subtle.exportKey(format, key);
        const bytes = format === "jwk" ? new TextEncoder().encode(JSON.stringify(exported)) : new Uint8Array(exported);
        const wa = typeof wrapAlg === "string" ? { name: wrapAlg } : wrapAlg;
        if (String(wa.name).toUpperCase() === "AES-KW") {
          const wd = keyStore.get(wrappingKey);
          return native("aes", "KW", true, wd.data, new Uint8Array(0), bytes, new Uint8Array(0), 0);
        }
        return subtle.encrypt(wrapAlg, wrappingKey, bytes);
      },
      async unwrapKey(format, wrapped, unwrappingKey, unwrapAlg, unwrappedAlg, extractable, usages) {
        const wa = typeof unwrapAlg === "string" ? { name: unwrapAlg } : unwrapAlg;
        let plain;
        if (String(wa.name).toUpperCase() === "AES-KW") {
          const wd = keyStore.get(unwrappingKey);
          plain = native("aes", "KW", false, wd.data, new Uint8Array(0), u8(wrapped), new Uint8Array(0), 0);
        } else {
          plain = await subtle.decrypt(unwrapAlg, unwrappingKey, wrapped);
        }
        const keyData = format === "jwk" ? JSON.parse(new TextDecoder().decode(new Uint8Array(plain))) : plain;
        return subtle.importKey(format, keyData, unwrappedAlg, extractable, usages);
      },
    };
    global.CryptoKey = CryptoKey;
    global.crypto = {
      subtle,
      getRandomValues(arr) { C("random", arr); return arr; },
      randomUUID() {
        const b = new Uint8Array(16);
        C("random", b);
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        const hex = Array.from(b, x => x.toString(16).padStart(2, "0")).join("");
        return hex.slice(0, 8) + "-" + hex.slice(8, 12) + "-" + hex.slice(12, 16) + "-" + hex.slice(16, 20) + "-" + hex.slice(20);
      },
    };
  }

  global.importScripts = function(...urls) {
    const base = typeof global.__fvWorkerUrl === "string" && global.__fvWorkerUrl ? global.__fvWorkerUrl : "chrome-extension://local/";
    for (const u of urls) {
      let target = String(u);
      try {
        const resolved = new URL(target, base);
        target = resolved.pathname.replace(/^\/+/, "");
      } catch (_) {}
      const code = __fvWReadFile(target);
      if (code === null) throw new DOMException("importScripts failed: " + u, "NetworkError");
      (0, eval)(code);
    }
  };

  global.__fvWDeliver = function(data) {
    global.dispatchEvent(new MessageEvent("message", { data }));
  };
})(globalThis);
)JS";

static void worker_thread_main(FvWorkerSlot* slot) {
  Platform* platform = static_cast<Platform*>(fv_v8_ensure_platform());
  slot->os_tid.store(get_os_tid(), std::memory_order_release);
  slot->start_ms = steady_now_ms();

  Isolate::CreateParams params;
  params.array_buffer_allocator = &slot->allocator;
  Isolate* iso = Isolate::New(params);

  {
    std::lock_guard<std::mutex> lk(slot->mu);
    slot->worker_isolate = iso;
    slot->ready.store(true, std::memory_order_release);
  }
  slot->cv.notify_all();

  {
    Isolate::Scope iso_scope(iso);
    HandleScope handle_scope(iso);
    tls_worker_slot = slot;
    iso->SetHostImportModuleDynamicallyCallback(worker_import_dynamic);
    iso->SetHostInitializeImportMetaObjectCallback(worker_init_import_meta);

    Local<External> ext = External::New(iso, slot);
    Local<ObjectTemplate> global_tmpl = ObjectTemplate::New(iso);
    global_tmpl->Set(iso, "__fvWPost", FunctionTemplate::New(iso, worker_native_post, ext));
    global_tmpl->Set(iso, "__fvWClose", FunctionTemplate::New(iso, worker_native_close, ext));
    global_tmpl->Set(iso, "__fvWNow", FunctionTemplate::New(iso, worker_native_now, ext));
    global_tmpl->Set(iso, "__fvWTid", FunctionTemplate::New(iso, worker_native_tid, ext));
    global_tmpl->Set(iso, "__fvWLog", FunctionTemplate::New(iso, worker_native_log, ext));
    global_tmpl->Set(iso, "__fvWSetTimer", FunctionTemplate::New(iso, worker_native_set_timer, ext));
    global_tmpl->Set(iso, "__fvWClearTimer", FunctionTemplate::New(iso, worker_native_clear_timer, ext));
    global_tmpl->Set(iso, "__fvWQueueMicrotask", FunctionTemplate::New(iso, worker_native_queue_microtask, ext));
    global_tmpl->Set(iso, "__fvWRegFile", FunctionTemplate::New(iso, worker_native_reg_file, ext));
    global_tmpl->Set(iso, "__fvWReadFile", FunctionTemplate::New(iso, worker_native_read_file, ext));
    global_tmpl->Set(iso, "__fvWRunModule", FunctionTemplate::New(iso, worker_native_run_module, ext));
    global_tmpl->Set(iso, "__fvWB64Decode", FunctionTemplate::New(iso, worker_native_b64_decode, ext));
    global_tmpl->Set(iso, "__fvWB64Encode", FunctionTemplate::New(iso, worker_native_b64_encode, ext));
    fv_crypto_install(iso, global_tmpl);

    Local<Context> ctx = Context::New(iso, nullptr, global_tmpl);
    Context::Scope ctx_scope(ctx);

    // Expose worker URL before running bootstrap.
    ctx->Global()
        ->Set(ctx, String::NewFromUtf8Literal(iso, "__fvWorkerUrl"),
              String::NewFromUtf8(iso, slot->url.data(), NewStringType::kNormal,
                                  static_cast<int>(slot->url.size()))
                  .ToLocalChecked())
        .Check();

    // Run bootstrap.
    {
      TryCatch tc(iso);
      Local<String> boot_src = String::NewFromUtf8(iso, kWorkerBootstrapJS).ToLocalChecked();
      Local<Script> boot_script;
      if (!Script::Compile(ctx, boot_src).ToLocal(&boot_script) ||
          boot_script->Run(ctx).IsEmpty()) {
        push_worker_error(slot, "Worker bootstrap failed: " + format_trycatch(iso, ctx, tc));
        slot->closed.store(true);
      }
    }

    // Cache __fvWDeliver function.
    Local<Function> deliver_fn;
    {
      Local<Value> v;
      if (ctx->Global()
              ->Get(ctx, String::NewFromUtf8Literal(iso, "__fvWDeliver"))
              .ToLocal(&v) &&
          v->IsFunction()) {
        deliver_fn = v.As<Function>();
      }
    }

    // Run initial worker script.
    if (!slot->closed.load() && !slot->stop_requested.load()) {
      TryCatch tc(iso);
      Local<String> src = String::NewFromUtf8(iso, slot->script.data(), NewStringType::kNormal,
                                              static_cast<int>(slot->script.size()))
                              .ToLocalChecked();
      Local<String> origin_str =
          String::NewFromUtf8(iso, slot->url.data(), NewStringType::kNormal,
                              static_cast<int>(slot->url.size()))
              .ToLocalChecked();
      ScriptOrigin origin(origin_str);
      Local<Script> script;
      if (!Script::Compile(ctx, src, &origin).ToLocal(&script) ||
          script->Run(ctx).IsEmpty()) {
        if (!slot->stop_requested.load()) {
          push_worker_error(slot, format_trycatch(iso, ctx, tc));
        }
      }
      iso->PerformMicrotaskCheckpoint();
    }

    // Main worker event loop.
    while (!slot->stop_requested.load() && !slot->closed.load()) {
      // 1. Pump V8 foreground tasks (e.g., async WebAssembly compilation) & microtasks.
      iso->PerformMicrotaskCheckpoint();
      while (!slot->stop_requested.load() && !slot->closed.load() &&
             fv_v8_pump_message_loop(platform, iso)) {
        iso->PerformMicrotaskCheckpoint();
      }
      if (slot->stop_requested.load() || slot->closed.load()) break;

      // 2. Fire due timers.
      double now = steady_now_ms();
      double next_deadline = -1.0;
      for (size_t i = 0; i < slot->timers.size();) {
        if (slot->stop_requested.load() || slot->closed.load()) break;
        if (slot->timers[i].fire_at_ms <= now) {
          Local<Function> fn = slot->timers[i].cb.Get(iso);
          bool repeat = slot->timers[i].repeat;
          if (repeat) {
            slot->timers[i].fire_at_ms = now + slot->timers[i].interval_ms;
            ++i;
          } else {
            slot->timers[i].cb.Reset();
            slot->timers.erase(slot->timers.begin() + static_cast<ptrdiff_t>(i));
          }
          if (!fn.IsEmpty()) {
            HandleScope hs(iso);
            TryCatch tc(iso);
            if (fn->Call(ctx, ctx->Global(), 0, nullptr).IsEmpty()) {
              if (!slot->stop_requested.load()) {
                push_worker_error(slot, format_trycatch(iso, ctx, tc));
              }
            }
            iso->PerformMicrotaskCheckpoint();
          }
        } else {
          if (next_deadline < 0.0 || slot->timers[i].fire_at_ms < next_deadline) {
            next_deadline = slot->timers[i].fire_at_ms;
          }
          ++i;
        }
      }
      if (slot->stop_requested.load() || slot->closed.load()) break;

      // 3. Pop next inbound message or wait briefly on cv.
      FvWorkerMessage in_msg;
      bool has_msg = false;
      {
        std::unique_lock<std::mutex> lk(slot->mu);
        if (!slot->inbound.empty()) {
          in_msg = std::move(slot->inbound.front());
          slot->inbound.pop_front();
          has_msg = true;
        } else if (!slot->stop_requested.load() && !slot->closed.load()) {
          double wait_ms = 5.0;
          if (next_deadline > 0.0) {
            double rem = next_deadline - steady_now_ms();
            if (rem < 0.0) rem = 0.0;
            if (rem < wait_ms) wait_ms = rem;
          }
          auto dur = std::chrono::microseconds(static_cast<int64_t>(wait_ms * 1000.0));
          slot->cv.wait_for(lk, dur, [&] {
            return slot->stop_requested.load() || slot->closed.load() || !slot->inbound.empty();
          });
          if (!slot->inbound.empty() && !slot->stop_requested.load() && !slot->closed.load()) {
            in_msg = std::move(slot->inbound.front());
            slot->inbound.pop_front();
            has_msg = true;
          }
        }
      }

      if (has_msg && !deliver_fn.IsEmpty()) {
        HandleScope hs(iso);
        TryCatch tc(iso);
        Local<Value> val;
        if (deserialize_v8_message(iso, ctx, in_msg).ToLocal(&val)) {
          Local<Value> argv[1] = {val};
          if (deliver_fn->Call(ctx, ctx->Global(), 1, argv).IsEmpty()) {
            if (!slot->stop_requested.load()) {
              push_worker_error(slot, format_trycatch(iso, ctx, tc));
            }
          }
        } else if (tc.HasCaught()) {
          push_worker_error(slot, format_trycatch(iso, ctx, tc));
        }
        iso->PerformMicrotaskCheckpoint();
      }
    }

    // Clear all Global timer & module handles before leaving Isolate::Scope.
    for (auto& t : slot->timers) {
      t.cb.Reset();
    }
    slot->timers.clear();
    for (auto& kv : slot->mod_cache) {
      if (kv.second) {
        kv.second->Reset();
        delete kv.second;
      }
    }
    slot->mod_cache.clear();
    slot->mod_urls.clear();
    slot->vfs.clear();
    tls_worker_slot = nullptr;
  }

  // Null out pointer under lock BEFORE disposing the Isolate on this exact OS thread.
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    slot->worker_isolate = nullptr;
  }
  iso->Dispose();
  slot->exited.store(true, std::memory_order_release);
}

// --- Main-Isolate V8 native bindings ---

static void main_worker_spawn(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  if (args.Length() < 1) {
    args.GetReturnValue().Set(Integer::New(iso, 0));
    return;
  }
  Local<String> s_code;
  if (!args[0]->ToString(ctx).ToLocal(&s_code)) {
    args.GetReturnValue().Set(Integer::New(iso, 0));
    return;
  }
  String::Utf8Value u_code(iso, s_code);
  std::string url = "about:blank";
  if (args.Length() > 1 && !args[1]->IsUndefined() && !args[1]->IsNull()) {
    Local<String> s_url;
    if (args[1]->ToString(ctx).ToLocal(&s_url)) {
      String::Utf8Value u_url(iso, s_url);
      if (*u_url) url.assign(*u_url, static_cast<size_t>(u_url.length()));
    }
  }
  int32_t id = fv_worker_spawn(*u_code ? *u_code : "", u_code.length(),
                               url.data(), static_cast<int32_t>(url.size()));
  args.GetReturnValue().Set(Integer::New(iso, id));
}

static void main_worker_post(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  if (args.Length() < 2) {
    args.GetReturnValue().Set(Boolean::New(iso, false));
    return;
  }
  int32_t id = args[0]->Int32Value(ctx).FromMaybe(0);
  if (id < 1 || id > FV_MAX_WORKERS) {
    args.GetReturnValue().Set(Boolean::New(iso, false));
    return;
  }
  FvWorkerSlot* slot = &g_slots[id - 1];
  if (!slot->in_use.load() || slot->stop_requested.load()) {
    args.GetReturnValue().Set(Boolean::New(iso, false));
    return;
  }
  Local<Value> transfer = args.Length() > 2 ? args[2] : Local<Value>(Undefined(iso));
  FvWorkerMessage msg;
  std::string err;
  if (!serialize_v8_message(iso, ctx, args[1], transfer, msg, err)) {
    iso->ThrowException(
        Exception::Error(String::NewFromUtf8(iso, err.c_str()).ToLocalChecked()));
    return;
  }
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    if (!slot->in_use.load() || slot->stop_requested.load()) {
      args.GetReturnValue().Set(Boolean::New(iso, false));
      return;
    }
    slot->inbound.push_back(std::move(msg));
  }
  slot->cv.notify_one();
  args.GetReturnValue().Set(Boolean::New(iso, true));
}

static void main_worker_poll(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  if (args.Length() < 1) {
    args.GetReturnValue().SetNull();
    return;
  }
  int32_t id = args[0]->Int32Value(ctx).FromMaybe(0);
  if (id < 1 || id > FV_MAX_WORKERS) {
    args.GetReturnValue().SetNull();
    return;
  }
  FvWorkerSlot* slot = &g_slots[id - 1];
  FvWorkerMessage msg;
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    if (slot->outbound.empty()) {
      args.GetReturnValue().SetNull();
      return;
    }
    msg = std::move(slot->outbound.front());
    slot->outbound.pop_front();
  }

  Local<Object> res = Object::New(iso);
  res->Set(ctx, String::NewFromUtf8Literal(iso, "kind"), Integer::New(iso, msg.kind)).Check();
  if (msg.kind == FV_WORKER_MSG_ERROR) {
    res->Set(ctx, String::NewFromUtf8Literal(iso, "message"),
             String::NewFromUtf8(iso, msg.text.data(), NewStringType::kNormal,
                                 static_cast<int>(msg.text.size()))
                 .ToLocalChecked())
        .Check();
  } else {
    Local<Value> data;
    if (deserialize_v8_message(iso, ctx, msg).ToLocal(&data)) {
      res->Set(ctx, String::NewFromUtf8Literal(iso, "data"), data).Check();
    } else {
      res->Set(ctx, String::NewFromUtf8Literal(iso, "data"), Undefined(iso)).Check();
    }
  }
  args.GetReturnValue().Set(res);
}

static void main_worker_terminate(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  if (args.Length() < 1) return;
  int32_t id = args[0]->Int32Value(ctx).FromMaybe(0);
  fv_worker_terminate(id);
}

static void main_worker_active_count(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(Integer::New(args.GetIsolate(), fv_worker_active_count()));
}

static void main_worker_main_tid(const FunctionCallbackInfo<Value>& args) {
  std::string s = std::to_string(fv_worker_main_thread_id());
  args.GetReturnValue().Set(
      String::NewFromUtf8(args.GetIsolate(), s.c_str()).ToLocalChecked());
}

static void main_worker_thread_id(const FunctionCallbackInfo<Value>& args) {
  Isolate* iso = args.GetIsolate();
  Local<Context> ctx = iso->GetCurrentContext();
  int32_t id = args.Length() > 0 ? args[0]->Int32Value(ctx).FromMaybe(0) : 0;
  std::string s = std::to_string(fv_worker_thread_id(id));
  args.GetReturnValue().Set(String::NewFromUtf8(iso, s.c_str()).ToLocalChecked());
}

}  // namespace

void fv_worker_install_main_bindings(Isolate* iso, Local<ObjectTemplate> global) {
  global->Set(iso, "__fvWorkerSpawn", FunctionTemplate::New(iso, main_worker_spawn));
  global->Set(iso, "__fvWorkerPost", FunctionTemplate::New(iso, main_worker_post));
  global->Set(iso, "__fvWorkerPoll", FunctionTemplate::New(iso, main_worker_poll));
  global->Set(iso, "__fvWorkerTerminate", FunctionTemplate::New(iso, main_worker_terminate));
  global->Set(iso, "__fvWorkerActiveCount", FunctionTemplate::New(iso, main_worker_active_count));
  global->Set(iso, "__fvWorkerMainTid", FunctionTemplate::New(iso, main_worker_main_tid));
  global->Set(iso, "__fvWorkerThreadId", FunctionTemplate::New(iso, main_worker_thread_id));
}

extern "C" {

int32_t fv_worker_spawn(const char* script_utf8, int32_t script_len,
                        const char* url_utf8, int32_t url_len) {
  fv_v8_ensure_platform();
  std::call_once(g_atexit_once, []() { std::atexit(fv_worker_terminate_all); });

  std::lock_guard<std::mutex> pool_lk(g_pool_mu);
  int slot_idx = -1;
  for (int i = 0; i < FV_MAX_WORKERS; ++i) {
    if (!g_slots[i].in_use.load() && !g_slots[i].terminating) {
      slot_idx = i;
      break;
    }
  }
  if (slot_idx < 0) return 0;

  FvWorkerSlot* slot = &g_slots[slot_idx];
  if (slot->thread.joinable()) {
    slot->thread.join();
  }
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    slot->inbound.clear();
    slot->outbound.clear();
    slot->timers.clear();
    slot->next_timer_id = 1;
    slot->worker_isolate = nullptr;
    slot->script.assign(script_utf8 ? script_utf8 : "",
                        script_len > 0 ? static_cast<size_t>(script_len) : 0);
    slot->url.assign(url_utf8 && url_len > 0 ? url_utf8 : "about:blank",
                     url_utf8 && url_len > 0 ? static_cast<size_t>(url_len) : 11);
    slot->stop_requested.store(false);
    slot->closed.store(false);
    slot->exited.store(false);
    slot->ready.store(false);
    slot->os_tid.store(0);
    slot->in_use.store(true);
  }
  g_active_count.fetch_add(1, std::memory_order_relaxed);
  slot->thread = std::thread(worker_thread_main, slot);

  // Wait briefly until worker thread has recorded its OS thread ID and created its Isolate.
  {
    std::unique_lock<std::mutex> lk(slot->mu);
    slot->cv.wait_for(lk, std::chrono::milliseconds(2000),
                      [&] { return slot->ready.load(std::memory_order_acquire); });
  }
  return slot_idx + 1;
}

int32_t fv_worker_post(int32_t id, const char* data_utf8, int32_t data_len) {
  if (id < 1 || id > FV_MAX_WORKERS) return 0;
  FvWorkerSlot* slot = &g_slots[id - 1];
  if (!slot->in_use.load() || slot->stop_requested.load()) return 0;
  FvWorkerMessage msg;
  msg.kind = 3;
  msg.text.assign(data_utf8 ? data_utf8 : "",
                  data_len > 0 ? static_cast<size_t>(data_len) : 0);
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    if (!slot->in_use.load() || slot->stop_requested.load()) return 0;
    slot->inbound.push_back(std::move(msg));
  }
  slot->cv.notify_one();
  return 1;
}

int32_t fv_worker_poll(int32_t id, char** out_utf8, int32_t* out_len) {
  if (out_utf8) *out_utf8 = nullptr;
  if (out_len) *out_len = 0;
  if (id < 1 || id > FV_MAX_WORKERS) return FV_WORKER_MSG_NONE;
  FvWorkerSlot* slot = &g_slots[id - 1];
  FvWorkerMessage msg;
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    if (slot->outbound.empty()) {
      if (!slot->in_use.load(std::memory_order_acquire) ||
          slot->exited.load(std::memory_order_acquire)) {
        return FV_WORKER_MSG_EXITED;
      }
      return FV_WORKER_MSG_NONE;
    }
    msg = std::move(slot->outbound.front());
    slot->outbound.pop_front();
  }
  if (out_utf8) {
    char* buf = static_cast<char*>(std::malloc(msg.text.size() + 1));
    if (buf) {
      if (!msg.text.empty()) std::memcpy(buf, msg.text.data(), msg.text.size());
      buf[msg.text.size()] = '\0';
      *out_utf8 = buf;
      if (out_len) *out_len = static_cast<int32_t>(msg.text.size());
    }
  }
  return msg.kind == FV_WORKER_MSG_ERROR ? FV_WORKER_MSG_ERROR : FV_WORKER_MSG_DATA;
}

void fv_worker_free(void* p) {
  std::free(p);
}

void fv_worker_terminate(int32_t id) {
  if (id < 1 || id > FV_MAX_WORKERS) return;
  FvWorkerSlot* slot = &g_slots[id - 1];
  std::thread to_join;
  {
    std::lock_guard<std::mutex> pool_lk(g_pool_mu);
    if (!slot->in_use.load() || slot->terminating) return;
    slot->terminating = true;
    {
      std::lock_guard<std::mutex> lk(slot->mu);
      slot->stop_requested.store(true);
      if (slot->worker_isolate) {
        slot->worker_isolate->TerminateExecution();
      }
    }
    slot->cv.notify_all();
    if (slot->thread.joinable()) {
      to_join = std::move(slot->thread);
    }
  }
  if (to_join.joinable()) {
    to_join.join();
  }
  {
    std::lock_guard<std::mutex> pool_lk(g_pool_mu);
    {
      std::lock_guard<std::mutex> lk(slot->mu);
      slot->inbound.clear();
      slot->outbound.clear();
      slot->os_tid.store(0);
      slot->in_use.store(false);
    }
    slot->terminating = false;
    g_active_count.fetch_sub(1, std::memory_order_relaxed);
  }
}

void fv_worker_terminate_all(void) {
  for (int i = 1; i <= FV_MAX_WORKERS; ++i) {
    fv_worker_terminate(i);
  }
}

int32_t fv_worker_active_count(void) {
  return g_active_count.load(std::memory_order_relaxed);
}

uint64_t fv_worker_thread_id(int32_t id) {
  if (id < 1 || id > FV_MAX_WORKERS) return 0;
  return g_slots[id - 1].os_tid.load(std::memory_order_acquire);
}

uint64_t fv_worker_main_thread_id(void) {
  return get_os_tid();
}

}  // extern "C"
