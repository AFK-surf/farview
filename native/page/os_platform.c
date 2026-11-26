// Farview OS-level platform integration layer:
//   1. Rich multi-format clipboard (macOS NSPasteboard via dlopen AppKit + ObjC runtime;
//      FV_CLIPBOARD_MEM=1 / Linux in-process fallback).
//   2. System proxy auto-detection & bypass rules (env vars + macOS CFNetwork dlopen).
//   3. Credential/cookie AES-256-GCM vault (BoringSSL from libcurl-impersonate) +
//      macOS Keychain via dlopen Security.framework (with FV_VAULT_TEST_KEY headless mode).
// Pure C, zero .m sources, zero new hard-linked frameworks, 0 warnings under -Wall -Wextra.

#if !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "os_platform.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "../win_compat.h"
#else
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <dlfcn.h>
#endif

/* ============================================================================
 * Portable ASCII helpers
 * ============================================================================ */

static int ascii_lower_ch(int c) {
  return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static int ascii_casecmp(const char *a, const char *b) {
  while (*a && *b) {
    int ca = ascii_lower_ch((unsigned char)*a);
    int cb = ascii_lower_ch((unsigned char)*b);
    if (ca != cb) return ca - cb;
    a++;
    b++;
  }
  return ascii_lower_ch((unsigned char)*a) - ascii_lower_ch((unsigned char)*b);
}

/* ============================================================================
 * Part 1: Rich Multi-Format Clipboard
 * ============================================================================ */

#define FV_CLIP_MAX_ITEMS 32
#define FV_CLIP_MAX_MIME  128

typedef struct {
  char mime[FV_CLIP_MAX_MIME];
  uint8_t *data;
  int32_t len;
} FvClipEntry;

static pthread_mutex_t g_clip_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_clip_mem_mode = 0;
static FvClipEntry g_clip_items[FV_CLIP_MAX_ITEMS];
static int32_t g_clip_count = 0;

void fv_clipboard_set_mem_mode(int32_t enabled) {
  pthread_mutex_lock(&g_clip_mu);
  g_clip_mem_mode = enabled ? 1 : 0;
  pthread_mutex_unlock(&g_clip_mu);
}

int32_t fv_clipboard_is_mem_mode(void) {
#ifndef __APPLE__
  return 1;
#else
  const char *env = getenv("FV_CLIPBOARD_MEM");
  if (env && env[0] == '1' && env[1] == '\0') return 1;
  pthread_mutex_lock(&g_clip_mu);
  int32_t m = g_clip_mem_mode;
  pthread_mutex_unlock(&g_clip_mu);
  return m;
#endif
}

static void clip_mem_clear_locked(void) {
  for (int32_t i = 0; i < g_clip_count; i++) {
    free(g_clip_items[i].data);
    g_clip_items[i].data = NULL;
    g_clip_items[i].len = 0;
    g_clip_items[i].mime[0] = '\0';
  }
  g_clip_count = 0;
}

static int32_t clip_mem_write(const char *mime, const void *data, int32_t len, int32_t clear_first) {
  if (!mime || !mime[0] || len < 0) return 0;
  pthread_mutex_lock(&g_clip_mu);
  if (clear_first) clip_mem_clear_locked();
  int32_t idx = -1;
  for (int32_t i = 0; i < g_clip_count; i++) {
    if (ascii_casecmp(g_clip_items[i].mime, mime) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    if (g_clip_count >= FV_CLIP_MAX_ITEMS) {
      pthread_mutex_unlock(&g_clip_mu);
      return 0;
    }
    idx = g_clip_count++;
    snprintf(g_clip_items[idx].mime, FV_CLIP_MAX_MIME, "%s", mime);
    g_clip_items[idx].data = NULL;
    g_clip_items[idx].len = 0;
  }
  uint8_t *copy = NULL;
  if (len > 0) {
    copy = (uint8_t *)malloc((size_t)len);
    if (!copy) {
      pthread_mutex_unlock(&g_clip_mu);
      return 0;
    }
    memcpy(copy, data, (size_t)len);
  }
  free(g_clip_items[idx].data);
  g_clip_items[idx].data = copy;
  g_clip_items[idx].len = len;
  pthread_mutex_unlock(&g_clip_mu);
  return 1;
}

static int32_t clip_mem_read(const char *mime, void *out_buf, int32_t cap) {
  if (!mime || !mime[0]) return -1;
  pthread_mutex_lock(&g_clip_mu);
  int32_t res = -1;
  for (int32_t i = 0; i < g_clip_count; i++) {
    if (ascii_casecmp(g_clip_items[i].mime, mime) == 0) {
      int32_t n = g_clip_items[i].len;
      if (!out_buf || cap <= 0) {
        res = n;
      } else {
        int32_t to_copy = n < cap ? n : cap;
        if (to_copy > 0 && g_clip_items[i].data) {
          memcpy(out_buf, g_clip_items[i].data, (size_t)to_copy);
        }
        res = to_copy;
      }
      break;
    }
  }
  pthread_mutex_unlock(&g_clip_mu);
  return res;
}

static int32_t clip_mem_types(char *out_buf, int32_t cap) {
  pthread_mutex_lock(&g_clip_mu);
  int32_t pos = 0;
  for (int32_t i = 0; i < g_clip_count; i++) {
    size_t ml = strlen(g_clip_items[i].mime);
    int32_t need = (int32_t)ml + (i > 0 ? 1 : 0);
    if (out_buf && cap > 0) {
      if (pos + need >= cap) break;
      if (i > 0) out_buf[pos++] = '\n';
      memcpy(out_buf + pos, g_clip_items[i].mime, ml);
      pos += (int32_t)ml;
    } else {
      pos += need;
    }
  }
  if (out_buf && cap > 0) out_buf[pos < cap ? pos : cap - 1] = '\0';
  pthread_mutex_unlock(&g_clip_mu);
  return pos;
}

#ifdef __APPLE__

typedef void *FvId;
typedef void *FvClass;
typedef void *FvSEL;

typedef FvId (*FvMsgSend0)(FvId, FvSEL);
typedef FvId (*FvMsgSend1)(FvId, FvSEL, FvId);
typedef int64_t (*FvMsgSend0I64)(FvId, FvSEL);
typedef unsigned long (*FvMsgSend0UL)(FvId, FvSEL);
typedef FvId (*FvMsgSend1UL)(FvId, FvSEL, unsigned long);
typedef const void *(*FvMsgSend0CPtr)(FvId, FvSEL);
typedef const char *(*FvMsgSend0CStr)(FvId, FvSEL);
typedef FvId (*FvMsgSend1CStr)(FvId, FvSEL, const char *);
typedef FvId (*FvMsgSendBytesLen)(FvId, FvSEL, const void *, unsigned long);
typedef bool (*FvMsgSendSetData)(FvId, FvSEL, FvId, FvId);

typedef struct {
  int initialized;
  int ok;
  void *h_objc;
  void *h_appkit;
  FvClass (*objc_getClass)(const char *name);
  FvSEL (*sel_registerName)(const char *str);
  void *objc_msgSend_ptr;
  void *(*autoreleasePoolPush)(void);
  void (*autoreleasePoolPop)(void *pool);
} FvAppKitAPI;

static FvAppKitAPI g_appkit = {0};
static pthread_mutex_t g_appkit_mu = PTHREAD_MUTEX_INITIALIZER;

static int load_appkit_api(void) {
  pthread_mutex_lock(&g_appkit_mu);
  if (g_appkit.initialized) {
    int ok = g_appkit.ok;
    pthread_mutex_unlock(&g_appkit_mu);
    return ok;
  }
  g_appkit.initialized = 1;
  g_appkit.h_objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);
  g_appkit.h_appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_LOCAL);
  if (g_appkit.h_objc && g_appkit.h_appkit) {
    g_appkit.objc_getClass = (FvClass (*)(const char *))dlsym(g_appkit.h_objc, "objc_getClass");
    g_appkit.sel_registerName = (FvSEL (*)(const char *))dlsym(g_appkit.h_objc, "sel_registerName");
    g_appkit.objc_msgSend_ptr = dlsym(g_appkit.h_objc, "objc_msgSend");
    g_appkit.autoreleasePoolPush = (void *(*)(void))dlsym(g_appkit.h_objc, "objc_autoreleasePoolPush");
    g_appkit.autoreleasePoolPop = (void (*)(void *))dlsym(g_appkit.h_objc, "objc_autoreleasePoolPop");
    if (g_appkit.objc_getClass && g_appkit.sel_registerName && g_appkit.objc_msgSend_ptr) {
      g_appkit.ok = 1;
    }
  }
  int ok = g_appkit.ok;
  pthread_mutex_unlock(&g_appkit_mu);
  return ok;
}

static const char *mime_to_uti(const char *mime) {
  if (ascii_casecmp(mime, "text/plain") == 0) return "public.utf8-plain-text";
  if (ascii_casecmp(mime, "text/html") == 0) return "public.html";
  if (ascii_casecmp(mime, "image/png") == 0) return "public.png";
  if (ascii_casecmp(mime, "text/uri-list") == 0) return "public.file-url";
  return mime;
}

static const char *uti_to_mime(const char *uti) {
  if (strcmp(uti, "public.utf8-plain-text") == 0 || strcmp(uti, "NSStringPboardType") == 0) return "text/plain";
  if (strcmp(uti, "public.html") == 0 || strcmp(uti, "Apple HTML pasteboard type") == 0) return "text/html";
  if (strcmp(uti, "public.png") == 0) return "image/png";
  if (strcmp(uti, "public.file-url") == 0 || strcmp(uti, "NSFilenamesPboardType") == 0) return "text/uri-list";
  return uti;
}

static FvId objc_nsstring(const char *utf8) {
  FvClass cls = g_appkit.objc_getClass("NSString");
  if (!cls) return NULL;
  FvSEL sel = g_appkit.sel_registerName("stringWithUTF8String:");
  return ((FvMsgSend1CStr)g_appkit.objc_msgSend_ptr)(cls, sel, utf8);
}

static FvId objc_general_pasteboard(void) {
  FvClass cls = g_appkit.objc_getClass("NSPasteboard");
  if (!cls) return NULL;
  FvSEL sel = g_appkit.sel_registerName("generalPasteboard");
  return ((FvMsgSend0)g_appkit.objc_msgSend_ptr)(cls, sel);
}

#endif /* __APPLE__ */

int32_t fv_clipboard_probe_os(void) {
#ifdef __APPLE__
  if (!load_appkit_api()) return 0;
  void *pool = g_appkit.autoreleasePoolPush ? g_appkit.autoreleasePoolPush() : NULL;
  FvId pb = objc_general_pasteboard();
  int32_t ok = (pb != NULL) ? 1 : 0;
  if (pool && g_appkit.autoreleasePoolPop) g_appkit.autoreleasePoolPop(pool);
  return ok;
#else
  return 0;
#endif
}

int32_t fv_clipboard_clear(void) {
  if (fv_clipboard_is_mem_mode()) {
    pthread_mutex_lock(&g_clip_mu);
    clip_mem_clear_locked();
    pthread_mutex_unlock(&g_clip_mu);
    return 1;
  }
#ifdef __APPLE__
  if (!load_appkit_api()) return 0;
  void *pool = g_appkit.autoreleasePoolPush ? g_appkit.autoreleasePoolPush() : NULL;
  FvId pb = objc_general_pasteboard();
  int32_t ok = 0;
  if (pb) {
    FvSEL sel = g_appkit.sel_registerName("clearContents");
    (void)((FvMsgSend0I64)g_appkit.objc_msgSend_ptr)(pb, sel);
    ok = 1;
  }
  if (pool && g_appkit.autoreleasePoolPop) g_appkit.autoreleasePoolPop(pool);
  return ok;
#else
  return 1;
#endif
}

int32_t fv_clipboard_write_item(const char *mime, const void *data, int32_t len, int32_t clear_first) {
  if (fv_clipboard_is_mem_mode()) {
    return clip_mem_write(mime, data, len, clear_first);
  }
#ifdef __APPLE__
  if (!mime || !mime[0] || len < 0 || !load_appkit_api()) return 0;
  void *pool = g_appkit.autoreleasePoolPush ? g_appkit.autoreleasePoolPush() : NULL;
  FvId pb = objc_general_pasteboard();
  int32_t ok = 0;
  if (pb) {
    if (clear_first) {
      FvSEL sel_clear = g_appkit.sel_registerName("clearContents");
      (void)((FvMsgSend0I64)g_appkit.objc_msgSend_ptr)(pb, sel_clear);
    }
    const char *uti = mime_to_uti(mime);
    FvId ns_type = objc_nsstring(uti);
    FvClass ns_data_cls = g_appkit.objc_getClass("NSData");
    FvSEL sel_data = g_appkit.sel_registerName("dataWithBytes:length:");
    static const uint8_t empty_byte = 0;
    const void *src = (len > 0 && data) ? data : &empty_byte;
    FvId ns_data = ns_data_cls
        ? ((FvMsgSendBytesLen)g_appkit.objc_msgSend_ptr)(ns_data_cls, sel_data, src, (unsigned long)len)
        : NULL;
    if (ns_type && ns_data) {
      FvSEL sel_set = g_appkit.sel_registerName("setData:forType:");
      ok = ((FvMsgSendSetData)g_appkit.objc_msgSend_ptr)(pb, sel_set, ns_data, ns_type) ? 1 : 0;
    }
  }
  if (pool && g_appkit.autoreleasePoolPop) g_appkit.autoreleasePoolPop(pool);
  return ok;
#else
  return clip_mem_write(mime, data, len, clear_first);
#endif
}

int32_t fv_clipboard_read(const char *mime, void *out_buf, int32_t cap) {
  if (fv_clipboard_is_mem_mode()) {
    return clip_mem_read(mime, out_buf, cap);
  }
#ifdef __APPLE__
  if (!mime || !mime[0] || !load_appkit_api()) return -1;
  void *pool = g_appkit.autoreleasePoolPush ? g_appkit.autoreleasePoolPush() : NULL;
  FvId pb = objc_general_pasteboard();
  int32_t res = -1;
  if (pb) {
    const char *uti = mime_to_uti(mime);
    FvId ns_type = objc_nsstring(uti);
    if (ns_type) {
      FvSEL sel_for_type = g_appkit.sel_registerName("dataForType:");
      FvId ns_data = ((FvMsgSend1)g_appkit.objc_msgSend_ptr)(pb, sel_for_type, ns_type);
      if (ns_data) {
        FvSEL sel_len = g_appkit.sel_registerName("length");
        FvSEL sel_bytes = g_appkit.sel_registerName("bytes");
        unsigned long n = ((FvMsgSend0UL)g_appkit.objc_msgSend_ptr)(ns_data, sel_len);
        const void *ptr = ((FvMsgSend0CPtr)g_appkit.objc_msgSend_ptr)(ns_data, sel_bytes);
        int32_t in_len = (n > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int32_t)n;
        if (!out_buf || cap <= 0) {
          res = in_len;
        } else {
          int32_t to_copy = in_len < cap ? in_len : cap;
          if (to_copy > 0 && ptr) memcpy(out_buf, ptr, (size_t)to_copy);
          res = to_copy;
        }
      }
    }
  }
  if (pool && g_appkit.autoreleasePoolPop) g_appkit.autoreleasePoolPop(pool);
  return res;
#else
  return clip_mem_read(mime, out_buf, cap);
#endif
}

int32_t fv_clipboard_types(char *out_buf, int32_t cap) {
  if (fv_clipboard_is_mem_mode()) {
    return clip_mem_types(out_buf, cap);
  }
#ifdef __APPLE__
  if (!load_appkit_api()) {
    if (out_buf && cap > 0) out_buf[0] = '\0';
    return 0;
  }
  void *pool = g_appkit.autoreleasePoolPush ? g_appkit.autoreleasePoolPush() : NULL;
  FvId pb = objc_general_pasteboard();
  int32_t pos = 0;
  const char *seen[64];
  int32_t seen_n = 0;
  if (pb) {
    FvSEL sel_types = g_appkit.sel_registerName("types");
    FvId arr = ((FvMsgSend0)g_appkit.objc_msgSend_ptr)(pb, sel_types);
    if (arr) {
      FvSEL sel_count = g_appkit.sel_registerName("count");
      FvSEL sel_at = g_appkit.sel_registerName("objectAtIndex:");
      FvSEL sel_utf8 = g_appkit.sel_registerName("UTF8String");
      unsigned long count = ((FvMsgSend0UL)g_appkit.objc_msgSend_ptr)(arr, sel_count);
      for (unsigned long i = 0; i < count && seen_n < 64; i++) {
        FvId s = ((FvMsgSend1UL)g_appkit.objc_msgSend_ptr)(arr, sel_at, i);
        if (!s) continue;
        const char *u = ((FvMsgSend0CStr)g_appkit.objc_msgSend_ptr)(s, sel_utf8);
        if (!u || !u[0]) continue;
        const char *m = uti_to_mime(u);
        int dup = 0;
        for (int32_t j = 0; j < seen_n; j++) {
          if (strcmp(seen[j], m) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        seen[seen_n++] = m;
        size_t ml = strlen(m);
        int32_t need = (int32_t)ml + (pos > 0 ? 1 : 0);
        if (out_buf && cap > 0) {
          if (pos + need >= cap) break;
          if (pos > 0) out_buf[pos++] = '\n';
          memcpy(out_buf + pos, m, ml);
          pos += (int32_t)ml;
        } else {
          pos += need;
        }
      }
    }
  }
  if (out_buf && cap > 0) out_buf[pos < cap ? pos : cap - 1] = '\0';
  if (pool && g_appkit.autoreleasePoolPop) g_appkit.autoreleasePoolPop(pool);
  return pos;
#else
  return clip_mem_types(out_buf, cap);
#endif
}

/* ============================================================================
 * Part 2: System Proxy Auto-Detection & Bypass Rules
 * ============================================================================ */

static int parse_ipv4_short(const char *s, uint32_t *out_addr) {
  /* Supports full a.b.c.d and macOS short-form a.b or a.b.c or a (for CIDR e.g. 169.254/16). */
  unsigned int octets[4] = {0, 0, 0, 0};
  int count = 0;
  const char *p = s;
  if (!*p) return 0;
  while (*p && count < 4) {
    if (!isdigit((unsigned char)*p)) return 0;
    unsigned int val = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
      val = val * 10u + (unsigned int)(*p - '0');
      if (val > 255u) return 0;
      digits++;
      p++;
    }
    if (digits == 0) return 0;
    octets[count++] = val;
    if (*p == '.') {
      p++;
      if (!*p) return 0;
    } else if (*p != '\0') {
      return 0;
    }
  }
  if (*p != '\0' || count == 0) return 0;
  *out_addr = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
  return 1;
}

static int parse_ipv4_full(const char *s, uint32_t *out_addr) {
  struct in_addr a;
  if (inet_pton(AF_INET, s, &a) != 1) return 0;
  *out_addr = ntohl(a.s_addr);
  return 1;
}

static int match_cidr_rule(const char *host, const char *rule) {
  const char *slash = strchr(rule, '/');
  if (!slash) return 0;
  char net_buf[128];
  size_t net_len = (size_t)(slash - rule);
  if (net_len == 0 || net_len >= sizeof(net_buf)) return 0;
  memcpy(net_buf, rule, net_len);
  net_buf[net_len] = '\0';

  const char *p = slash + 1;
  if (!*p) return 0;
  int prefix = 0;
  while (*p) {
    if (!isdigit((unsigned char)*p)) return 0;
    prefix = prefix * 10 + (*p - '0');
    if (prefix > 128) return 0;
    p++;
  }

  /* IPv6 CIDR */
  if (strchr(net_buf, ':') != NULL) {
    struct in6_addr h6, n6;
    if (inet_pton(AF_INET6, host, &h6) != 1) return 0;
    if (inet_pton(AF_INET6, net_buf, &n6) != 1) return 0;
    if (prefix > 128) return 0;
    int full_bytes = prefix / 8;
    int rem_bits = prefix % 8;
    if (full_bytes > 0 && memcmp(h6.s6_addr, n6.s6_addr, (size_t)full_bytes) != 0) return 0;
    if (rem_bits > 0) {
      uint8_t mask = (uint8_t)(0xFFu << (8 - rem_bits));
      if ((h6.s6_addr[full_bytes] & mask) != (n6.s6_addr[full_bytes] & mask)) return 0;
    }
    return 1;
  }

  /* IPv4 CIDR (including macOS shorthand like 169.254/16) */
  if (prefix > 32) return 0;
  uint32_t h4 = 0, n4 = 0;
  if (!parse_ipv4_full(host, &h4)) return 0;
  if (!parse_ipv4_short(net_buf, &n4)) return 0;
  if (prefix == 0) return 1;
  uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
  return (h4 & mask) == (n4 & mask);
}

int32_t fv_proxy_host_matches_bypass(const char *host, const char *rule) {
  if (!host || !host[0] || !rule) return 0;

  /* Trim whitespace on rule */
  while (*rule && isspace((unsigned char)*rule)) rule++;
  size_t rlen = strlen(rule);
  while (rlen > 0 && isspace((unsigned char)rule[rlen - 1])) rlen--;
  if (rlen == 0) return 0;

  char rbuf[256];
  if (rlen >= sizeof(rbuf)) return 0;
  memcpy(rbuf, rule, rlen);
  rbuf[rlen] = '\0';

  /* Strip brackets on host if IPv6 literal */
  char hbuf[256];
  size_t hlen = strlen(host);
  if (hlen >= sizeof(hbuf)) return 0;
  if (hlen >= 2 && host[0] == '[' && host[hlen - 1] == ']') {
    memcpy(hbuf, host + 1, hlen - 2);
    hbuf[hlen - 2] = '\0';
    hlen -= 2;
  } else {
    memcpy(hbuf, host, hlen + 1);
  }

  /* Strip brackets on rule if IPv6 literal like [::1] */
  if (rlen >= 2 && rbuf[0] == '[' && rbuf[rlen - 1] == ']') {
    memmove(rbuf, rbuf + 1, rlen - 2);
    rbuf[rlen - 2] = '\0';
    rlen -= 2;
  }

  if (strcmp(rbuf, "*") == 0) return 1;

  if (ascii_casecmp(rbuf, "<local>") == 0) {
    return (strchr(hbuf, '.') == NULL && strchr(hbuf, ':') == NULL) ? 1 : 0;
  }

  /* CIDR rule */
  if (strchr(rbuf, '/') != NULL) {
    return match_cidr_rule(hbuf, rbuf) ? 1 : 0;
  }

  /* Strip optional :port from non-IPv6 rule (e.g. localhost:8080) */
  char *colon = strchr(rbuf, ':');
  if (colon && strchr(colon + 1, ':') == NULL) {
    *colon = '\0';
    rlen = strlen(rbuf);
  }

  /* Exact match */
  if (ascii_casecmp(hbuf, rbuf) == 0) return 1;

  /* Wildcard prefix: *.example.com or .example.com */
  const char *suffix = NULL;
  int match_bare = 0;
  if (rlen >= 2 && rbuf[0] == '*' && rbuf[1] == '.') {
    suffix = rbuf + 1; /* ".example.com" */
    match_bare = 0;
  } else if (rlen >= 2 && rbuf[0] == '.') {
    suffix = rbuf; /* ".example.com" */
    match_bare = 1;
  }
  if (suffix) {
    size_t slen = strlen(suffix);
    if (hlen > slen && ascii_casecmp(hbuf + (hlen - slen), suffix) == 0) return 1;
    if (match_bare && ascii_casecmp(hbuf, suffix + 1) == 0) return 1;
    return 0;
  }

  /* Suffix match for bare domain rule in NO_PROXY (e.g. "example.com" matches "sub.example.com") */
  if (hlen > rlen && hbuf[hlen - rlen - 1] == '.' && ascii_casecmp(hbuf + (hlen - rlen), rbuf) == 0) {
    return 1;
  }

  return 0;
}

static int extract_url_scheme_and_host(const char *url,
                                       char *scheme_out, size_t scheme_cap,
                                       char *host_out, size_t host_cap) {
  if (!url || !url[0]) return 0;
  const char *sep = strstr(url, "://");
  if (!sep) return 0;
  size_t slen = (size_t)(sep - url);
  if (slen == 0 || slen >= scheme_cap) return 0;
  for (size_t i = 0; i < slen; i++) {
    scheme_out[i] = (char)ascii_lower_ch((unsigned char)url[i]);
  }
  scheme_out[slen] = '\0';

  const char *auth = sep + 3;
  const char *end = auth;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;

  /* Skip userinfo if present */
  const char *at = NULL;
  for (const char *p = auth; p < end; p++) {
    if (*p == '@') at = p;
  }
  const char *hstart = at ? (at + 1) : auth;
  const char *hend = end;
  if (hstart >= hend) return 0;

  if (*hstart == '[') {
    const char *rb = memchr(hstart, ']', (size_t)(hend - hstart));
    if (!rb) return 0;
    hstart++;
    hend = rb;
  } else {
    const char *colon = memchr(hstart, ':', (size_t)(hend - hstart));
    if (colon) hend = colon;
  }

  size_t hlen = (size_t)(hend - hstart);
  if (hlen == 0 || hlen >= host_cap) return 0;
  memcpy(host_out, hstart, hlen);
  host_out[hlen] = '\0';
  return 1;
}

int32_t fv_proxy_url_is_bypassed(const char *url, const char *no_proxy_list) {
  if (!url || !no_proxy_list || !no_proxy_list[0]) return 0;
  char scheme[32], host[256];
  if (!extract_url_scheme_and_host(url, scheme, sizeof(scheme), host, sizeof(host))) return 0;

  const char *p = no_proxy_list;
  while (*p) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    if (len > 0 && len < 256) {
      char rule[256];
      memcpy(rule, p, len);
      rule[len] = '\0';
      if (fv_proxy_host_matches_bypass(host, rule)) return 1;
    }
    if (!comma) break;
    p = comma + 1;
  }
  return 0;
}

static int32_t write_proxy_uri(const char *raw, char *out, int32_t cap) {
  if (!raw || !raw[0]) {
    if (out && cap > 0) out[0] = '\0';
    return 0;
  }
  while (*raw && isspace((unsigned char)*raw)) raw++;
  size_t len = strlen(raw);
  while (len > 0 && isspace((unsigned char)raw[len - 1])) len--;
  if (len == 0) {
    if (out && cap > 0) out[0] = '\0';
    return 0;
  }
  int has_scheme = 0;
  for (size_t i = 0; i + 2 < len; i++) {
    if (raw[i] == ':' && raw[i + 1] == '/' && raw[i + 2] == '/') {
      has_scheme = 1;
      break;
    }
  }
  int written;
  if (has_scheme) {
    written = snprintf(out, (size_t)cap, "%.*s", (int)len, raw);
  } else {
    written = snprintf(out, (size_t)cap, "http://%.*s", (int)len, raw);
  }
  if (written < 0) return -1;
  if (written >= cap) return cap - 1;
  return (int32_t)written;
}

#ifdef __APPLE__

typedef const void *FvCFTypeRef;
typedef const void *FvCFStringRef;
typedef const void *FvCFDictionaryRef;
typedef const void *FvCFArrayRef;
typedef const void *FvCFNumberRef;
typedef const void *FvCFDataRef;
typedef long FvCFIndex;

#define FV_CF_STR_UTF8  0x08000100u
#define FV_CF_NUM_SINT32 3

typedef struct {
  int initialized;
  int cf_ok;
  int cfn_ok;
  int sec_ok;
  void *h_cf;
  void *h_cfn;
  void *h_sec;

  /* CoreFoundation */
  FvCFStringRef (*CFStringCreateWithCString)(const void *alloc, const char *cStr, uint32_t encoding);
  bool (*CFStringGetCString)(FvCFStringRef str, char *buf, FvCFIndex bufSize, uint32_t encoding);
  const void *(*CFDictionaryGetValue)(FvCFDictionaryRef dict, const void *key);
  FvCFDictionaryRef (*CFDictionaryCreate)(const void *alloc, const void **keys, const void **values,
                                          FvCFIndex numValues, const void *keyCallBacks, const void *valCallBacks);
  FvCFIndex (*CFArrayGetCount)(FvCFArrayRef arr);
  const void *(*CFArrayGetValueAtIndex)(FvCFArrayRef arr, FvCFIndex idx);
  bool (*CFNumberGetValue)(FvCFNumberRef num, int type, void *valuePtr);
  FvCFDataRef (*CFDataCreate)(const void *alloc, const uint8_t *bytes, FvCFIndex length);
  FvCFIndex (*CFDataGetLength)(FvCFDataRef data);
  const uint8_t *(*CFDataGetBytePtr)(FvCFDataRef data);
  unsigned long (*CFGetTypeID)(FvCFTypeRef cf);
  unsigned long (*CFStringGetTypeID)(void);
  unsigned long (*CFNumberGetTypeID)(void);
  unsigned long (*CFArrayGetTypeID)(void);
  unsigned long (*CFDataGetTypeID)(void);
  void (*CFRelease)(FvCFTypeRef cf);
  const void *kCFTypeDictionaryKeyCallBacks;
  const void *kCFTypeDictionaryValueCallBacks;
  FvCFTypeRef kCFBooleanTrue;

  /* CFNetwork */
  FvCFDictionaryRef (*CFNetworkCopySystemProxySettings)(void);

  /* Security.framework Keychain */
  int32_t (*SecItemCopyMatching)(FvCFDictionaryRef query, FvCFTypeRef *result);
  int32_t (*SecItemAdd)(FvCFDictionaryRef attributes, FvCFTypeRef *result);
  int32_t (*SecItemDelete)(FvCFDictionaryRef query);
  FvCFTypeRef kSecClass;
  FvCFTypeRef kSecClassGenericPassword;
  FvCFTypeRef kSecAttrService;
  FvCFTypeRef kSecAttrAccount;
  FvCFTypeRef kSecValueData;
  FvCFTypeRef kSecReturnData;
  FvCFTypeRef kSecMatchLimit;
  FvCFTypeRef kSecMatchLimitOne;
} FvAppleSysAPI;

static FvAppleSysAPI g_sys = {0};
static pthread_mutex_t g_sys_mu = PTHREAD_MUTEX_INITIALIZER;

static FvCFTypeRef dlsym_cf_const(void *h, const char *name) {
  const void *const *p = (const void *const *)dlsym(h, name);
  return p ? *p : NULL;
}

static void load_apple_sys_api(void) {
  pthread_mutex_lock(&g_sys_mu);
  if (g_sys.initialized) {
    pthread_mutex_unlock(&g_sys_mu);
    return;
  }
  g_sys.initialized = 1;
  g_sys.h_cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
  if (g_sys.h_cf) {
    g_sys.CFStringCreateWithCString =
        (FvCFStringRef (*)(const void *, const char *, uint32_t))dlsym(g_sys.h_cf, "CFStringCreateWithCString");
    g_sys.CFStringGetCString =
        (bool (*)(FvCFStringRef, char *, FvCFIndex, uint32_t))dlsym(g_sys.h_cf, "CFStringGetCString");
    g_sys.CFDictionaryGetValue =
        (const void *(*)(FvCFDictionaryRef, const void *))dlsym(g_sys.h_cf, "CFDictionaryGetValue");
    g_sys.CFDictionaryCreate =
        (FvCFDictionaryRef (*)(const void *, const void **, const void **, FvCFIndex, const void *, const void *))
            dlsym(g_sys.h_cf, "CFDictionaryCreate");
    g_sys.CFArrayGetCount = (FvCFIndex (*)(FvCFArrayRef))dlsym(g_sys.h_cf, "CFArrayGetCount");
    g_sys.CFArrayGetValueAtIndex =
        (const void *(*)(FvCFArrayRef, FvCFIndex))dlsym(g_sys.h_cf, "CFArrayGetValueAtIndex");
    g_sys.CFNumberGetValue = (bool (*)(FvCFNumberRef, int, void *))dlsym(g_sys.h_cf, "CFNumberGetValue");
    g_sys.CFDataCreate = (FvCFDataRef (*)(const void *, const uint8_t *, FvCFIndex))dlsym(g_sys.h_cf, "CFDataCreate");
    g_sys.CFDataGetLength = (FvCFIndex (*)(FvCFDataRef))dlsym(g_sys.h_cf, "CFDataGetLength");
    g_sys.CFDataGetBytePtr = (const uint8_t *(*)(FvCFDataRef))dlsym(g_sys.h_cf, "CFDataGetBytePtr");
    g_sys.CFGetTypeID = (unsigned long (*)(FvCFTypeRef))dlsym(g_sys.h_cf, "CFGetTypeID");
    g_sys.CFStringGetTypeID = (unsigned long (*)(void))dlsym(g_sys.h_cf, "CFStringGetTypeID");
    g_sys.CFNumberGetTypeID = (unsigned long (*)(void))dlsym(g_sys.h_cf, "CFNumberGetTypeID");
    g_sys.CFArrayGetTypeID = (unsigned long (*)(void))dlsym(g_sys.h_cf, "CFArrayGetTypeID");
    g_sys.CFDataGetTypeID = (unsigned long (*)(void))dlsym(g_sys.h_cf, "CFDataGetTypeID");
    g_sys.CFRelease = (void (*)(FvCFTypeRef))dlsym(g_sys.h_cf, "CFRelease");
    g_sys.kCFTypeDictionaryKeyCallBacks = dlsym(g_sys.h_cf, "kCFTypeDictionaryKeyCallBacks");
    g_sys.kCFTypeDictionaryValueCallBacks = dlsym(g_sys.h_cf, "kCFTypeDictionaryValueCallBacks");
    g_sys.kCFBooleanTrue = dlsym_cf_const(g_sys.h_cf, "kCFBooleanTrue");

    if (g_sys.CFStringCreateWithCString && g_sys.CFStringGetCString && g_sys.CFDictionaryGetValue &&
        g_sys.CFArrayGetCount && g_sys.CFArrayGetValueAtIndex && g_sys.CFNumberGetValue &&
        g_sys.CFGetTypeID && g_sys.CFStringGetTypeID && g_sys.CFNumberGetTypeID &&
        g_sys.CFArrayGetTypeID && g_sys.CFRelease) {
      g_sys.cf_ok = 1;
    }
  }

  g_sys.h_cfn = dlopen("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", RTLD_LAZY | RTLD_LOCAL);
  if (g_sys.cf_ok && g_sys.h_cfn) {
    g_sys.CFNetworkCopySystemProxySettings =
        (FvCFDictionaryRef (*)(void))dlsym(g_sys.h_cfn, "CFNetworkCopySystemProxySettings");
    if (g_sys.CFNetworkCopySystemProxySettings) g_sys.cfn_ok = 1;
  }

  g_sys.h_sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY | RTLD_LOCAL);
  if (g_sys.cf_ok && g_sys.h_sec && g_sys.CFDictionaryCreate && g_sys.CFDataCreate &&
      g_sys.CFDataGetLength && g_sys.CFDataGetBytePtr && g_sys.CFDataGetTypeID && g_sys.kCFBooleanTrue) {
    g_sys.SecItemCopyMatching =
        (int32_t (*)(FvCFDictionaryRef, FvCFTypeRef *))dlsym(g_sys.h_sec, "SecItemCopyMatching");
    g_sys.SecItemAdd = (int32_t (*)(FvCFDictionaryRef, FvCFTypeRef *))dlsym(g_sys.h_sec, "SecItemAdd");
    g_sys.SecItemDelete = (int32_t (*)(FvCFDictionaryRef))dlsym(g_sys.h_sec, "SecItemDelete");
    g_sys.kSecClass = dlsym_cf_const(g_sys.h_sec, "kSecClass");
    g_sys.kSecClassGenericPassword = dlsym_cf_const(g_sys.h_sec, "kSecClassGenericPassword");
    g_sys.kSecAttrService = dlsym_cf_const(g_sys.h_sec, "kSecAttrService");
    g_sys.kSecAttrAccount = dlsym_cf_const(g_sys.h_sec, "kSecAttrAccount");
    g_sys.kSecValueData = dlsym_cf_const(g_sys.h_sec, "kSecValueData");
    g_sys.kSecReturnData = dlsym_cf_const(g_sys.h_sec, "kSecReturnData");
    g_sys.kSecMatchLimit = dlsym_cf_const(g_sys.h_sec, "kSecMatchLimit");
    g_sys.kSecMatchLimitOne = dlsym_cf_const(g_sys.h_sec, "kSecMatchLimitOne");
    if (g_sys.SecItemCopyMatching && g_sys.SecItemAdd && g_sys.SecItemDelete &&
        g_sys.kSecClass && g_sys.kSecClassGenericPassword && g_sys.kSecAttrService &&
        g_sys.kSecAttrAccount && g_sys.kSecValueData && g_sys.kSecReturnData &&
        g_sys.kSecMatchLimit && g_sys.kSecMatchLimitOne) {
      g_sys.sec_ok = 1;
    }
  }
  pthread_mutex_unlock(&g_sys_mu);
}

static int cf_dict_get_int(FvCFDictionaryRef dict, const char *key_cstr, int32_t *out_val) {
  FvCFStringRef k = g_sys.CFStringCreateWithCString(NULL, key_cstr, FV_CF_STR_UTF8);
  if (!k) return 0;
  FvCFTypeRef v = g_sys.CFDictionaryGetValue(dict, k);
  g_sys.CFRelease(k);
  if (!v || g_sys.CFGetTypeID(v) != g_sys.CFNumberGetTypeID()) return 0;
  return g_sys.CFNumberGetValue(v, FV_CF_NUM_SINT32, out_val) ? 1 : 0;
}

static int cf_dict_get_str(FvCFDictionaryRef dict, const char *key_cstr, char *buf, size_t cap) {
  FvCFStringRef k = g_sys.CFStringCreateWithCString(NULL, key_cstr, FV_CF_STR_UTF8);
  if (!k) return 0;
  FvCFTypeRef v = g_sys.CFDictionaryGetValue(dict, k);
  g_sys.CFRelease(k);
  if (!v || g_sys.CFGetTypeID(v) != g_sys.CFStringGetTypeID()) return 0;
  return g_sys.CFStringGetCString(v, buf, (FvCFIndex)cap, FV_CF_STR_UTF8) ? 1 : 0;
}

static int32_t macos_system_proxy_for_host(const char *scheme, const char *host,
                                           char *out_buf, int32_t cap) {
  load_apple_sys_api();
  if (!g_sys.cfn_ok) return 0;
  FvCFDictionaryRef dict = g_sys.CFNetworkCopySystemProxySettings();
  if (!dict) return 0;

  /* Check ExcludeSimpleHostnames */
  int32_t exclude_simple = 0;
  if (cf_dict_get_int(dict, "ExcludeSimpleHostnames", &exclude_simple) && exclude_simple) {
    if (strchr(host, '.') == NULL && strchr(host, ':') == NULL) {
      g_sys.CFRelease(dict);
      return 0;
    }
  }

  /* Check ExceptionsList */
  FvCFStringRef exc_key = g_sys.CFStringCreateWithCString(NULL, "ExceptionsList", FV_CF_STR_UTF8);
  if (exc_key) {
    FvCFTypeRef exc = g_sys.CFDictionaryGetValue(dict, exc_key);
    g_sys.CFRelease(exc_key);
    if (exc && g_sys.CFGetTypeID(exc) == g_sys.CFArrayGetTypeID()) {
      FvCFIndex n = g_sys.CFArrayGetCount(exc);
      for (FvCFIndex i = 0; i < n; i++) {
        FvCFTypeRef item = g_sys.CFArrayGetValueAtIndex(exc, i);
        if (item && g_sys.CFGetTypeID(item) == g_sys.CFStringGetTypeID()) {
          char rule[256];
          if (g_sys.CFStringGetCString(item, rule, (FvCFIndex)sizeof(rule), FV_CF_STR_UTF8)) {
            if (fv_proxy_host_matches_bypass(host, rule)) {
              g_sys.CFRelease(dict);
              return 0;
            }
          }
        }
      }
    }
  }

  int is_https = (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0);
  int is_http = (strcmp(scheme, "http") == 0 || strcmp(scheme, "ws") == 0);
  int32_t enable = 0, port = 0;
  char proxy_host[256];
  int32_t written = 0;

  if (is_https && cf_dict_get_int(dict, "HTTPSEnable", &enable) && enable &&
      cf_dict_get_str(dict, "HTTPSProxy", proxy_host, sizeof(proxy_host)) && proxy_host[0]) {
    if (!cf_dict_get_int(dict, "HTTPSPort", &port) || port <= 0) port = 443;
    int w = snprintf(out_buf, (size_t)cap, "http://%s:%d", proxy_host, (int)port);
    written = (w < 0) ? 0 : (w >= cap ? cap - 1 : (int32_t)w);
  } else if (is_http && cf_dict_get_int(dict, "HTTPEnable", &enable) && enable &&
             cf_dict_get_str(dict, "HTTPProxy", proxy_host, sizeof(proxy_host)) && proxy_host[0]) {
    if (!cf_dict_get_int(dict, "HTTPPort", &port) || port <= 0) port = 80;
    int w = snprintf(out_buf, (size_t)cap, "http://%s:%d", proxy_host, (int)port);
    written = (w < 0) ? 0 : (w >= cap ? cap - 1 : (int32_t)w);
  } else if (cf_dict_get_int(dict, "SOCKSEnable", &enable) && enable &&
             cf_dict_get_str(dict, "SOCKSProxy", proxy_host, sizeof(proxy_host)) && proxy_host[0]) {
    if (!cf_dict_get_int(dict, "SOCKSPort", &port) || port <= 0) port = 1080;
    int w = snprintf(out_buf, (size_t)cap, "socks5h://%s:%d", proxy_host, (int)port);
    written = (w < 0) ? 0 : (w >= cap ? cap - 1 : (int32_t)w);
  }

  g_sys.CFRelease(dict);
  return written;
}

#endif /* __APPLE__ */

static const char *fv_getenv_exact(const char *name) {
#ifdef _WIN32
  if (!name || !name[0] || !_environ) return NULL;
  size_t nlen = strlen(name);
  for (char **ep = _environ; *ep; ep++) {
    if (strncmp(*ep, name, nlen) == 0 && (*ep)[nlen] == '=') {
      return *ep + nlen + 1;
    }
  }
  return NULL;
#else
  return getenv(name);
#endif
}

int32_t fv_system_proxy_for_url(const char *url, char *out_proxy_buf, int32_t cap) {
  if (!out_proxy_buf || cap <= 0) return -1;
  out_proxy_buf[0] = '\0';
  if (!url || !url[0]) return 0;

  char scheme[32], host[256];
  if (!extract_url_scheme_and_host(url, scheme, sizeof(scheme), host, sizeof(host))) {
    return 0;
  }

  /* 1. Respect NO_PROXY (case-sensitive) first */
  const char *no_proxy = fv_getenv_exact("NO_PROXY");
  if (no_proxy && no_proxy[0] && fv_proxy_url_is_bypassed(url, no_proxy)) {
    return 0;
  }

  /* 2. Respect standard uppercase proxy env vars (case-sensitive) */
  const char *env_proxy = NULL;
  if (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0) {
    env_proxy = fv_getenv_exact("HTTPS_PROXY");
    if (!env_proxy || !env_proxy[0]) env_proxy = fv_getenv_exact("ALL_PROXY");
  } else if (strcmp(scheme, "http") == 0 || strcmp(scheme, "ws") == 0) {
    env_proxy = fv_getenv_exact("HTTP_PROXY");
    if (!env_proxy || !env_proxy[0]) env_proxy = fv_getenv_exact("ALL_PROXY");
  } else {
    env_proxy = fv_getenv_exact("ALL_PROXY");
  }
  if (env_proxy && env_proxy[0]) {
    return write_proxy_uri(env_proxy, out_proxy_buf, cap);
  }

  /* Optional escape hatch for tests to bypass host macOS proxy settings */
  const char *ignore_sys = getenv("FV_IGNORE_SYSTEM_PROXY");
  if (ignore_sys && ignore_sys[0] == '1' && ignore_sys[1] == '\0') {
    return 0;
  }

  /* 3. macOS CFNetwork system proxy settings */
#ifdef __APPLE__
  return macos_system_proxy_for_host(scheme, host, out_proxy_buf, cap);
#else
  return 0;
#endif
}

/* ============================================================================
 * Part 3: AES-256-GCM Credential Vault (BoringSSL) + macOS Keychain (dlopen)
 * ============================================================================ */

typedef struct evp_aead_st EVP_AEAD;
typedef struct evp_aead_ctx_st EVP_AEAD_CTX;

extern const EVP_AEAD *EVP_aead_aes_256_gcm(void);
extern EVP_AEAD_CTX *EVP_AEAD_CTX_new(const EVP_AEAD *aead, const uint8_t *key, size_t key_len, size_t tag_len);
extern void EVP_AEAD_CTX_free(EVP_AEAD_CTX *ctx);
extern int EVP_AEAD_CTX_seal(const EVP_AEAD_CTX *ctx,
                             uint8_t *out, size_t *out_len, size_t max_out_len,
                             const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *in, size_t in_len,
                             const uint8_t *ad, size_t ad_len);
extern int EVP_AEAD_CTX_open(const EVP_AEAD_CTX *ctx,
                             uint8_t *out, size_t *out_len, size_t max_out_len,
                             const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *in, size_t in_len,
                             const uint8_t *ad, size_t ad_len);
extern int RAND_bytes(uint8_t *buf, size_t len);
extern uint8_t *SHA256(const uint8_t *data, size_t len, uint8_t *out);

#define FV_VAULT_NONCE_LEN 12
#define FV_VAULT_TAG_LEN   16
#define FV_VAULT_KEY_LEN   32

int32_t fv_vault_seal(const uint8_t *key32,
                      const uint8_t *plaintext, int32_t pt_len,
                      const uint8_t *aad, int32_t aad_len,
                      uint8_t *out_buf, int32_t out_cap) {
  if (!key32 || pt_len < 0 || aad_len < 0 || !out_buf) return -1;
  int32_t need = FV_VAULT_NONCE_LEN + pt_len + FV_VAULT_TAG_LEN;
  if (out_cap < need) return -1;

  uint8_t nonce[FV_VAULT_NONCE_LEN];
  if (RAND_bytes(nonce, FV_VAULT_NONCE_LEN) != 1) {
#ifdef _WIN32
    return -1;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, nonce, FV_VAULT_NONCE_LEN);
    close(fd);
    if (r != FV_VAULT_NONCE_LEN) return -1;
#endif
  }

  EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new(EVP_aead_aes_256_gcm(), key32, FV_VAULT_KEY_LEN, FV_VAULT_TAG_LEN);
  if (!ctx) return -1;

  static const uint8_t empty_b = 0;
  const uint8_t *in_ptr = (pt_len > 0 && plaintext) ? plaintext : &empty_b;
  const uint8_t *ad_ptr = (aad_len > 0 && aad) ? aad : &empty_b;

  memcpy(out_buf, nonce, FV_VAULT_NONCE_LEN);
  size_t sealed_len = 0;
  int ok = EVP_AEAD_CTX_seal(ctx,
                             out_buf + FV_VAULT_NONCE_LEN,
                             &sealed_len,
                             (size_t)(out_cap - FV_VAULT_NONCE_LEN),
                             nonce, FV_VAULT_NONCE_LEN,
                             in_ptr, (size_t)pt_len,
                             ad_ptr, (size_t)aad_len);
  EVP_AEAD_CTX_free(ctx);
  if (ok != 1) return -1;
  return (int32_t)(FV_VAULT_NONCE_LEN + sealed_len);
}

int32_t fv_vault_open(const uint8_t *key32,
                      const uint8_t *sealed, int32_t sealed_len,
                      const uint8_t *aad, int32_t aad_len,
                      uint8_t *out_buf, int32_t out_cap) {
  if (!key32 || !sealed || sealed_len < FV_VAULT_NONCE_LEN + FV_VAULT_TAG_LEN || aad_len < 0) {
    return -1;
  }
  int32_t expected_pt = sealed_len - FV_VAULT_NONCE_LEN - FV_VAULT_TAG_LEN;
  if (out_cap < expected_pt) return -1;

  EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new(EVP_aead_aes_256_gcm(), key32, FV_VAULT_KEY_LEN, FV_VAULT_TAG_LEN);
  if (!ctx) return -1;

  static const uint8_t empty_b = 0;
  uint8_t dummy_out = 0;
  uint8_t *dst = (out_buf && out_cap > 0) ? out_buf : &dummy_out;
  size_t dst_cap = (out_buf && out_cap > 0) ? (size_t)out_cap : 1u;
  const uint8_t *ad_ptr = (aad_len > 0 && aad) ? aad : &empty_b;

  size_t pt_len = 0;
  int ok = EVP_AEAD_CTX_open(ctx,
                             dst, &pt_len, dst_cap,
                             sealed, FV_VAULT_NONCE_LEN,
                             sealed + FV_VAULT_NONCE_LEN, (size_t)(sealed_len - FV_VAULT_NONCE_LEN),
                             ad_ptr, (size_t)aad_len);
  EVP_AEAD_CTX_free(ctx);
  if (ok != 1) {
    if (out_buf && out_cap > 0) memset(out_buf, 0, (size_t)out_cap);
    return -1;
  }
  return (int32_t)pt_len;
}

int32_t fv_keychain_probe_os(void) {
#ifdef __APPLE__
  load_apple_sys_api();
  return g_sys.sec_ok ? 1 : 0;
#else
  return 0;
#endif
}

int32_t fv_keychain_delete(const char *service, const char *account) {
#ifdef __APPLE__
  if (!service || !account) return 0;
  load_apple_sys_api();
  if (!g_sys.sec_ok) return 0;
  FvCFStringRef s_svc = g_sys.CFStringCreateWithCString(NULL, service, FV_CF_STR_UTF8);
  FvCFStringRef s_acc = g_sys.CFStringCreateWithCString(NULL, account, FV_CF_STR_UTF8);
  if (!s_svc || !s_acc) {
    if (s_svc) g_sys.CFRelease(s_svc);
    if (s_acc) g_sys.CFRelease(s_acc);
    return 0;
  }
  const void *keys[3] = { g_sys.kSecClass, g_sys.kSecAttrService, g_sys.kSecAttrAccount };
  const void *vals[3] = { g_sys.kSecClassGenericPassword, s_svc, s_acc };
  FvCFDictionaryRef q = g_sys.CFDictionaryCreate(
      NULL, keys, vals, 3, g_sys.kCFTypeDictionaryKeyCallBacks, g_sys.kCFTypeDictionaryValueCallBacks);
  g_sys.CFRelease(s_svc);
  g_sys.CFRelease(s_acc);
  if (!q) return 0;
  int32_t status = g_sys.SecItemDelete(q);
  g_sys.CFRelease(q);
  /* errSecSuccess (0) or errSecItemNotFound (-25300) */
  return (status == 0 || status == -25300) ? 1 : 0;
#else
  (void)service;
  (void)account;
  return 0;
#endif
}

int32_t fv_keychain_write(const char *service, const char *account,
                          const uint8_t *data, int32_t len) {
#ifdef __APPLE__
  if (!service || !account || !data || len <= 0) return 0;
  load_apple_sys_api();
  if (!g_sys.sec_ok) return 0;
  (void)fv_keychain_delete(service, account);

  FvCFStringRef s_svc = g_sys.CFStringCreateWithCString(NULL, service, FV_CF_STR_UTF8);
  FvCFStringRef s_acc = g_sys.CFStringCreateWithCString(NULL, account, FV_CF_STR_UTF8);
  FvCFDataRef d_val = g_sys.CFDataCreate(NULL, data, (FvCFIndex)len);
  if (!s_svc || !s_acc || !d_val) {
    if (s_svc) g_sys.CFRelease(s_svc);
    if (s_acc) g_sys.CFRelease(s_acc);
    if (d_val) g_sys.CFRelease(d_val);
    return 0;
  }
  const void *keys[4] = { g_sys.kSecClass, g_sys.kSecAttrService, g_sys.kSecAttrAccount, g_sys.kSecValueData };
  const void *vals[4] = { g_sys.kSecClassGenericPassword, s_svc, s_acc, d_val };
  FvCFDictionaryRef attrs = g_sys.CFDictionaryCreate(
      NULL, keys, vals, 4, g_sys.kCFTypeDictionaryKeyCallBacks, g_sys.kCFTypeDictionaryValueCallBacks);
  g_sys.CFRelease(s_svc);
  g_sys.CFRelease(s_acc);
  g_sys.CFRelease(d_val);
  if (!attrs) return 0;
  int32_t status = g_sys.SecItemAdd(attrs, NULL);
  g_sys.CFRelease(attrs);
  return (status == 0) ? 1 : 0;
#else
  (void)service;
  (void)account;
  (void)data;
  (void)len;
  return 0;
#endif
}

int32_t fv_keychain_read(const char *service, const char *account,
                         uint8_t *out_buf, int32_t cap) {
#ifdef __APPLE__
  if (!service || !account || !out_buf || cap <= 0) return -1;
  load_apple_sys_api();
  if (!g_sys.sec_ok) return -1;

  FvCFStringRef s_svc = g_sys.CFStringCreateWithCString(NULL, service, FV_CF_STR_UTF8);
  FvCFStringRef s_acc = g_sys.CFStringCreateWithCString(NULL, account, FV_CF_STR_UTF8);
  if (!s_svc || !s_acc) {
    if (s_svc) g_sys.CFRelease(s_svc);
    if (s_acc) g_sys.CFRelease(s_acc);
    return -1;
  }
  const void *keys[5] = {
    g_sys.kSecClass, g_sys.kSecAttrService, g_sys.kSecAttrAccount,
    g_sys.kSecReturnData, g_sys.kSecMatchLimit
  };
  const void *vals[5] = {
    g_sys.kSecClassGenericPassword, s_svc, s_acc,
    g_sys.kCFBooleanTrue, g_sys.kSecMatchLimitOne
  };
  FvCFDictionaryRef q = g_sys.CFDictionaryCreate(
      NULL, keys, vals, 5, g_sys.kCFTypeDictionaryKeyCallBacks, g_sys.kCFTypeDictionaryValueCallBacks);
  g_sys.CFRelease(s_svc);
  g_sys.CFRelease(s_acc);
  if (!q) return -1;

  FvCFTypeRef result = NULL;
  int32_t status = g_sys.SecItemCopyMatching(q, &result);
  g_sys.CFRelease(q);
  if (status != 0 || !result) return -1;

  int32_t copied = -1;
  if (g_sys.CFGetTypeID(result) == g_sys.CFDataGetTypeID()) {
    FvCFIndex n = g_sys.CFDataGetLength(result);
    const uint8_t *ptr = g_sys.CFDataGetBytePtr(result);
    if (n >= 0 && ptr) {
      int32_t to_copy = (n > (FvCFIndex)cap) ? cap : (int32_t)n;
      memcpy(out_buf, ptr, (size_t)to_copy);
      copied = to_copy;
    }
  }
  g_sys.CFRelease(result);
  return copied;
#else
  (void)service;
  (void)account;
  (void)out_buf;
  (void)cap;
  return -1;
#endif
}

static int hex_nibble(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int32_t fv_vault_get_or_create_master_key(const char *service, const char *account,
                                          uint8_t out_key32[32]) {
  if (!out_key32) return 0;

  /* Deterministic headless CI override — never triggers macOS Keychain popup */
  const char *test_key = getenv("FV_VAULT_TEST_KEY");
  if (test_key && test_key[0]) {
    size_t len = strlen(test_key);
    if (len == 64) {
      int all_hex = 1;
      uint8_t decoded[32];
      for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)test_key[i * 2]);
        int lo = hex_nibble((unsigned char)test_key[i * 2 + 1]);
        if (hi < 0 || lo < 0) { all_hex = 0; break; }
        decoded[i] = (uint8_t)((hi << 4) | lo);
      }
      if (all_hex) {
        memcpy(out_key32, decoded, 32);
        return 1;
      }
    }
    SHA256((const uint8_t *)test_key, len, out_key32);
    return 1;
  }

  if (!service || !service[0] || !account || !account[0]) return 0;

#ifdef __APPLE__
  int32_t got = fv_keychain_read(service, account, out_key32, 32);
  if (got == 32) return 1;
  uint8_t fresh[32];
  if (RAND_bytes(fresh, 32) != 1) return 0;
  if (fv_keychain_write(service, account, fresh, 32)) {
    memcpy(out_key32, fresh, 32);
    return 1;
  }
#endif
  return 0;
}

int32_t fv_os_setenv(const char *name, const char *val) {
  if (!name || !name[0] || !val) return 0;
  return setenv(name, val, 1) == 0 ? 1 : 0;
}

int32_t fv_os_unsetenv(const char *name) {
  if (!name || !name[0]) return 0;
  return unsetenv(name) == 0 ? 1 : 0;
}
