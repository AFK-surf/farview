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
  std::atomic<bool> ready{false};
  std::atomic<uint64_t> os_tid{0};

  // Protected by `mu`: pointer valid only while the worker thread's Isolate is alive.
  Isolate* worker_isolate = nullptr;

  std::string script;
  std::string url;
  double start_ms = 0.0;

  std::deque<FvWorkerMessage> inbound;
  std::deque<FvWorkerMessage> outbound;

  // Worker-thread-only timer state (accessed strictly on the worker OS thread).
  int32_t next_timer_id = 1;
  std::vector<FvWorkerTimer> timers;

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
    (void)ab->Detach(Local<Value>());
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

  global.navigator = { hardwareConcurrency: 4, userAgent: "Farview" };
  global.location = { href: global.__fvWorkerUrl || "about:blank" };

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
             platform::PumpMessageLoop(platform, iso)) {
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

    // Clear all Global timer handles before leaving Isolate::Scope.
    for (auto& t : slot->timers) {
      t.cb.Reset();
    }
    slot->timers.clear();
  }

  // Null out pointer under lock BEFORE disposing the Isolate on this exact OS thread.
  {
    std::lock_guard<std::mutex> lk(slot->mu);
    slot->worker_isolate = nullptr;
  }
  iso->Dispose();
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
    if (slot->outbound.empty()) return FV_WORKER_MSG_NONE;
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
