// HTTP through libcurl: the body, and the URL it ended at.
#include <curl/curl.h>

// curl-impersonate's own call (deps/curl-impersonate, linked in place of the system's libcurl)
CURL_EXTERN CURLcode curl_easy_impersonate(CURL* curl, const char* target, int default_headers);
#include <moonbit.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#ifdef _WIN32
#include "../win_compat.h"
#include <psapi.h>
volatile void *RtlSetVolatileMemory(volatile void *dst, int fill, size_t len) {
  volatile unsigned char *p = (volatile unsigned char *)dst;
  unsigned char b = (unsigned char)fill;
  for (size_t i = 0; i < len; i++) p[i] = b;
  return dst;
}
#else
#include <arpa/inet.h>
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#include <stdbool.h>
#endif
#ifdef __APPLE__
// What curl-impersonate and the system's decoders need on macOS, said in this object (Mach-O's LC_LINKER_OPTION, as
// hostfont's coretext.c does): no program's moon.pkg names a framework, so the same link flags build on Linux.
__asm__(".linker_option \"-lc++\"\n"
        ".linker_option \"-framework\", \"Security\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n"
        ".linker_option \"-framework\", \"CoreServices\"\n"
        ".linker_option \"-framework\", \"ImageIO\"\n"
        ".linker_option \"-framework\", \"CoreGraphics\"\n"
        ".linker_option \"-licucore\"\n"
        ".linker_option \"-liconv\"\n");
#endif

typedef struct { char* p; size_t n; } Buf;

static size_t take(char* d, size_t s, size_t m, void* u) {
  Buf* b = u;
  size_t k = s * m;
  b->p = realloc(b->p, b->n + k);
  memcpy(b->p + b->n, d, k);
  b->n += k;
  return k;
}

static char last_url[4096];

// whether the host is Chrome on a phone (else on a desktop)
static int phone = 0;

void fv_set_phone(int32_t on) { phone = on; }

static char last_ctype[512];
// the last get's Content-Disposition ("attachment; filename=...": a file to save, not a page to show)
static char last_disposition[1024];
// one handle for every request: its connections stay open for the next request to the same host
static CURL* handle = 0;

static void setup(CURL* c, const char* url, Buf* b, int creds);

moonbit_bytes_t fv_get(moonbit_bytes_t url) {
  Buf b = {0, 0};
  if (!handle) handle = curl_easy_init();
  CURL* c = handle;
  curl_easy_reset(c);
  setup(c, (const char*)url, &b, 1);
  last_url[0] = 0;
  // what failed part way (a timeout, a dropped connection) is no answer: a cut-off script is not a script
  last_ctype[0] = 0;
  last_disposition[0] = 0;
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    if (getenv("FV_FETCH_LOG")) fprintf(stderr, "get failed (%s) %s\n", curl_easy_strerror(rc), (const char*)url);
    b.n = 0;
  } else {
    char* ct = 0;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) { strncpy(last_ctype, ct, sizeof last_ctype - 1); last_ctype[sizeof last_ctype - 1] = 0; }
    struct curl_header* cd = 0;
    if (curl_easy_header(c, "Content-Disposition", 0, CURLH_HEADER, -1, &cd) == CURLHE_OK && cd && cd->value) {
      strncpy(last_disposition, cd->value, sizeof last_disposition - 1);
      last_disposition[sizeof last_disposition - 1] = 0;
    }
    if (getenv("FV_FETCH_LOG")) {
      double t = 0;
      curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &t);
      fprintf(stderr, "get %.2fs %zu %s\n", t, b.n, (const char*)url);
    }
    char* u = 0;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &u);
    if (u) strncpy(last_url, u, sizeof last_url - 1);
  }
  moonbit_bytes_t out = moonbit_make_bytes(b.n, 0);
  if (b.n) memcpy(out, b.p, b.n);
  free(b.p);
  return out;
}

// one cookie jar (and DNS, TLS sessions and connections) for every request: a page's logins stay on the host, as a browser's do;
// FV_COOKIE_JAR keeps them in a file between runs
static CURLSH* share = 0;
static void lock_cb(CURL* h, curl_lock_data d, curl_lock_access a, void* u) { (void)h; (void)d; (void)a; (void)u; }

static CURLSH* the_share(void) {
  if (!share) {
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    // and one pool of open connections: a batch of prefetches, a script's request and the next get all reuse what
    // is open to a host (each batch opened its own and closed them, a TLS handshake to the far side of the world
    // each time). Every request runs on this one thread, so the no-op locks are enough.
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock_cb);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, lock_cb);
  }
  return share;
}

CURLSH* fv_the_share(void) { return the_share(); }

void fv_apply_connect_to(CURL* c) {
  static struct curl_slist* connect_to = 0;
  const char* to = getenv("FV_CONNECT_TO");
  if (to && *to) {
    if (!connect_to) connect_to = curl_slist_append(0, to);
    curl_easy_setopt(c, CURLOPT_CONNECT_TO, connect_to);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  }
}

// the same without cookies, for a request a page sends without credentials (a script's to another site, unless it
// asks for them): no cookie goes with it and none it answers with is kept
static CURLSH* bare_share = 0;
static CURLSH* the_bare_share(void) {
  if (!bare_share) {
    bare_share = curl_share_init();
    curl_share_setopt(bare_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(bare_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(bare_share, CURLSHOPT_LOCKFUNC, lock_cb);
    curl_share_setopt(bare_share, CURLSHOPT_UNLOCKFUNC, lock_cb);
  }
  return bare_share;
}

// A phone's Chrome as sites see it: the current Chrome's handshakes with Android's user agent and client hints
// (curl-impersonate's own Android profile is an old Chrome, which some sites turn away)
#define PHONE_UA "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Mobile Safari/537.36"
static struct curl_slist* phone_headers(struct curl_slist* hs) {
  hs = curl_slist_append(hs, "sec-ch-ua-mobile: ?1");
  hs = curl_slist_append(hs, "sec-ch-ua-platform: \"Android\"");
  return hs;
}

// The page what is fetched is for: sent as the Referer a browser sends (strict-origin-when-cross-origin, Chrome's
// default): the whole address to its own origin, the origin alone to another, nothing from https to http. CDNs turn
// away requests without one (sspai's post.sspai.com answers 403).
static char referer[4096];

void fv_set_referer(moonbit_bytes_t page) {
  strncpy(referer, (const char*)page, sizeof referer - 1);
  char* hash = strchr(referer, '#');
  if (hash) *hash = 0;
}

static size_t origin_len(const char* u) {
  const char* p = strstr(u, "://");
  if (!p) return 0;
  const char* e = strchr(p + 3, '/');
  return e ? (size_t)(e - u) : strlen(u);
}

static void set_referer(CURL* c, const char* url) {
  if (!referer[0] || strncmp(referer, "http", 4) != 0) return;
  if (strncmp(referer, "https:", 6) == 0 && strncmp(url, "http:", 5) == 0) return;
  size_t n = origin_len(referer);
  if (n && origin_len(url) == n && strncmp(url, referer, n) == 0) {
    curl_easy_setopt(c, CURLOPT_REFERER, referer);
  } else if (n) {
    char o[4096];
    snprintf(o, sizeof o, "%.*s/", (int)n, referer);
    curl_easy_setopt(c, CURLOPT_REFERER, o);
  }
}

// Whether what is asked for now is a picture (an img's, a background's): asked for with an Accept of the formats every
// host draws, not Chrome's own (which names AVIF and WebP: CDNs answer even a .jpg with them for it, and a Linux host
// draws neither). A document, a script or a page's own request keeps Chrome's.
static int picture_accept = 0;
#define PICTURE_ACCEPT "Accept: image/webp,image/png,image/jpeg,image/gif,image/svg+xml,image/*;q=0.8,*/*;q=0.5"

void fv_set_picture_accept(int32_t on) { picture_accept = on; }

static int is_loopback_url(const char* url);

static int fetch_ascii_lower(int c) {
  return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static int fetch_ascii_casecmp(const char* a, const char* b) {
  while (*a && *b) {
    int ca = fetch_ascii_lower((unsigned char)*a);
    int cb = fetch_ascii_lower((unsigned char)*b);
    if (ca != cb) return ca - cb;
    a++;
    b++;
  }
  return fetch_ascii_lower((unsigned char)*a) - fetch_ascii_lower((unsigned char)*b);
}

static int fetch_parse_ipv4_short(const char* s, uint32_t* out_addr) {
  unsigned int octets[4] = {0, 0, 0, 0};
  int count = 0;
  const char* p = s;
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

static int fetch_host_matches_bypass(const char* host, const char* rule) {
  if (!host || !host[0] || !rule) return 0;
  while (*rule && isspace((unsigned char)*rule)) rule++;
  size_t rlen = strlen(rule);
  while (rlen > 0 && isspace((unsigned char)rule[rlen - 1])) rlen--;
  if (rlen == 0 || rlen >= 256) return 0;
  char rbuf[256];
  memcpy(rbuf, rule, rlen);
  rbuf[rlen] = '\0';

  if (strcmp(rbuf, "*") == 0) return 1;
  const char* slash = strchr(rbuf, '/');
  if (slash) {
    char net_buf[128];
    size_t nlen = (size_t)(slash - rbuf);
    if (nlen == 0 || nlen >= sizeof(net_buf)) return 0;
    memcpy(net_buf, rbuf, nlen);
    net_buf[nlen] = '\0';
    const char* p = slash + 1;
    if (!*p) return 0;
    int prefix = 0;
    while (*p) {
      if (!isdigit((unsigned char)*p)) return 0;
      prefix = prefix * 10 + (*p - '0');
      if (prefix > 128) return 0;
      p++;
    }
    if (strchr(net_buf, ':')) {
      struct in6_addr h6, n6;
      if (inet_pton(AF_INET6, host, &h6) != 1 || inet_pton(AF_INET6, net_buf, &n6) != 1) return 0;
      int fb = prefix / 8, rb = prefix % 8;
      if (fb > 0 && memcmp(h6.s6_addr, n6.s6_addr, (size_t)fb) != 0) return 0;
      if (rb > 0) {
        uint8_t m = (uint8_t)(0xFFu << (8 - rb));
        if ((h6.s6_addr[fb] & m) != (n6.s6_addr[fb] & m)) return 0;
      }
      return 1;
    }
    if (prefix > 32) return 0;
    struct in_addr ha;
    uint32_t n4 = 0;
    if (inet_pton(AF_INET, host, &ha) != 1 || !fetch_parse_ipv4_short(net_buf, &n4)) return 0;
    uint32_t h4 = ntohl(ha.s_addr);
    if (prefix == 0) return 1;
    uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
    return (h4 & mask) == (n4 & mask);
  }
  if (fetch_ascii_casecmp(host, rbuf) == 0) return 1;
  size_t hlen = strlen(host);
  if (rlen >= 2 && rbuf[0] == '*' && rbuf[1] == '.') {
    const char* suf = rbuf + 1;
    size_t slen = rlen - 1;
    return (hlen > slen && fetch_ascii_casecmp(host + (hlen - slen), suf) == 0) ? 1 : 0;
  }
  if (rlen >= 2 && rbuf[0] == '.') {
    if (hlen > rlen && fetch_ascii_casecmp(host + (hlen - rlen), rbuf) == 0) return 1;
    return fetch_ascii_casecmp(host, rbuf + 1) == 0 ? 1 : 0;
  }
  if (hlen > rlen && host[hlen - rlen - 1] == '.' && fetch_ascii_casecmp(host + (hlen - rlen), rbuf) == 0) {
    return 1;
  }
  return 0;
}

static int fetch_extract_scheme_host(const char* url, char* scheme, size_t scap, char* host, size_t hcap) {
  if (!url) return 0;
  const char* sep = strstr(url, "://");
  if (!sep) return 0;
  size_t slen = (size_t)(sep - url);
  if (slen == 0 || slen >= scap) return 0;
  for (size_t i = 0; i < slen; i++) scheme[i] = (char)fetch_ascii_lower((unsigned char)url[i]);
  scheme[slen] = '\0';
  const char* auth = sep + 3;
  const char* end = auth;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  const char* at = NULL;
  for (const char* p = auth; p < end; p++) if (*p == '@') at = p;
  const char* hs = at ? at + 1 : auth;
  const char* he = end;
  if (hs >= he) return 0;
  if (*hs == '[') {
    const char* rb = memchr(hs, ']', (size_t)(he - hs));
    if (!rb) return 0;
    hs++;
    he = rb;
  } else {
    const char* colon = memchr(hs, ':', (size_t)(he - hs));
    if (colon) he = colon;
  }
  size_t hlen = (size_t)(he - hs);
  if (hlen == 0 || hlen >= hcap) return 0;
  memcpy(host, hs, hlen);
  host[hlen] = '\0';
  return 1;
}

#ifdef __APPLE__
static int fetch_macos_cfnetwork_proxy(const char* scheme, const char* host, char* out, size_t cap) {
  static int inited = 0;
  static const void* (*p_copy_settings)(void) = NULL;
  static const void* (*p_str_create)(const void*, const char*, uint32_t) = NULL;
  static bool (*p_str_get)(const void*, char*, long, uint32_t) = NULL;
  static const void* (*p_dict_get)(const void*, const void*) = NULL;
  static long (*p_arr_count)(const void*) = NULL;
  static const void* (*p_arr_at)(const void*, long) = NULL;
  static bool (*p_num_get)(const void*, int, void*) = NULL;
  static unsigned long (*p_type_id)(const void*) = NULL;
  static unsigned long (*p_str_tid)(void) = NULL;
  static unsigned long (*p_num_tid)(void) = NULL;
  static unsigned long (*p_arr_tid)(void) = NULL;
  static void (*p_release)(const void*) = NULL;
  if (!inited) {
    inited = 1;
    void* h_cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
    void* h_cfn = dlopen("/System/Library/Frameworks/CFNetwork.framework/CFNetwork", RTLD_LAZY | RTLD_LOCAL);
    if (h_cf && h_cfn) {
      p_copy_settings = (const void* (*)(void))dlsym(h_cfn, "CFNetworkCopySystemProxySettings");
      p_str_create = (const void* (*)(const void*, const char*, uint32_t))dlsym(h_cf, "CFStringCreateWithCString");
      p_str_get = (bool (*)(const void*, char*, long, uint32_t))dlsym(h_cf, "CFStringGetCString");
      p_dict_get = (const void* (*)(const void*, const void*))dlsym(h_cf, "CFDictionaryGetValue");
      p_arr_count = (long (*)(const void*))dlsym(h_cf, "CFArrayGetCount");
      p_arr_at = (const void* (*)(const void*, long))dlsym(h_cf, "CFArrayGetValueAtIndex");
      p_num_get = (bool (*)(const void*, int, void*))dlsym(h_cf, "CFNumberGetValue");
      p_type_id = (unsigned long (*)(const void*))dlsym(h_cf, "CFGetTypeID");
      p_str_tid = (unsigned long (*)(void))dlsym(h_cf, "CFStringGetTypeID");
      p_num_tid = (unsigned long (*)(void))dlsym(h_cf, "CFNumberGetTypeID");
      p_arr_tid = (unsigned long (*)(void))dlsym(h_cf, "CFArrayGetTypeID");
      p_release = (void (*)(const void*))dlsym(h_cf, "CFRelease");
    }
  }
  if (!p_copy_settings || !p_str_create || !p_str_get || !p_dict_get || !p_arr_count ||
      !p_arr_at || !p_num_get || !p_type_id || !p_str_tid || !p_num_tid || !p_arr_tid || !p_release) {
    return 0;
  }
  const void* dict = p_copy_settings();
  if (!dict) return 0;

  const uint32_t kUTF8 = 0x08000100u;
  const void* k_exc = p_str_create(NULL, "ExceptionsList", kUTF8);
  if (k_exc) {
    const void* exc = p_dict_get(dict, k_exc);
    p_release(k_exc);
    if (exc && p_type_id(exc) == p_arr_tid()) {
      long n = p_arr_count(exc);
      for (long i = 0; i < n; i++) {
        const void* item = p_arr_at(exc, i);
        if (item && p_type_id(item) == p_str_tid()) {
          char rule[256];
          if (p_str_get(item, rule, (long)sizeof(rule), kUTF8) && fetch_host_matches_bypass(host, rule)) {
            p_release(dict);
            return 0;
          }
        }
      }
    }
  }

  const char* en_key = NULL;
  const char* host_key = NULL;
  const char* port_key = NULL;
  const char* proto = "http";
  int def_port = 80;
  int32_t en = 0, port = 0;
  char phost[256];
  int found = 0;

  const char* tries[2][4] = {
    { (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0) ? "HTTPSEnable" : "HTTPEnable",
      (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0) ? "HTTPSProxy" : "HTTPProxy",
      (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0) ? "HTTPSPort" : "HTTPPort",
      "http" },
    { "SOCKSEnable", "SOCKSProxy", "SOCKSPort", "socks5h" }
  };
  for (int t = 0; t < 2 && !found; t++) {
    en_key = tries[t][0]; host_key = tries[t][1]; port_key = tries[t][2]; proto = tries[t][3];
    def_port = (t == 1) ? 1080 : ((strcmp(scheme, "https") == 0) ? 443 : 80);
    en = 0; port = 0; phost[0] = '\0';
    const void* ke = p_str_create(NULL, en_key, kUTF8);
    const void* kh = p_str_create(NULL, host_key, kUTF8);
    const void* kp = p_str_create(NULL, port_key, kUTF8);
    const void* ve = ke ? p_dict_get(dict, ke) : NULL;
    const void* vh = kh ? p_dict_get(dict, kh) : NULL;
    const void* vp = kp ? p_dict_get(dict, kp) : NULL;
    if (ve && p_type_id(ve) == p_num_tid() && p_num_get(ve, 3, &en) && en &&
        vh && p_type_id(vh) == p_str_tid() && p_str_get(vh, phost, (long)sizeof(phost), kUTF8) && phost[0]) {
      if (!(vp && p_type_id(vp) == p_num_tid() && p_num_get(vp, 3, &port) && port > 0)) port = def_port;
      snprintf(out, cap, "%s://%s:%d", proto, phost, (int)port);
      found = 1;
    }
    if (ke) p_release(ke);
    if (kh) p_release(kh);
    if (kp) p_release(kp);
  }
  p_release(dict);
  return found;
}
#endif

static int fv_fetch_resolve_system_proxy(const char* url, char* out, size_t cap) {
  if (!url || !out || cap == 0) return 0;
  out[0] = '\0';
  if (is_loopback_url(url)) return 0;

  char scheme[32], host[256];
  if (!fetch_extract_scheme_host(url, scheme, sizeof(scheme), host, sizeof(host))) return 0;

  const char* no_proxy = getenv("NO_PROXY");
  if (no_proxy && no_proxy[0]) {
    const char* p = no_proxy;
    while (*p) {
      const char* comma = strchr(p, ',');
      size_t len = comma ? (size_t)(comma - p) : strlen(p);
      if (len > 0 && len < 256) {
        char rule[256];
        memcpy(rule, p, len);
        rule[len] = '\0';
        if (fetch_host_matches_bypass(host, rule)) return 0;
      }
      if (!comma) break;
      p = comma + 1;
    }
  }

  const char* env_proxy = NULL;
  if (strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0) {
    env_proxy = getenv("HTTPS_PROXY");
    if (!env_proxy || !env_proxy[0]) env_proxy = getenv("ALL_PROXY");
  } else if (strcmp(scheme, "http") == 0 || strcmp(scheme, "ws") == 0) {
    env_proxy = getenv("HTTP_PROXY");
    if (!env_proxy || !env_proxy[0]) env_proxy = getenv("ALL_PROXY");
  } else {
    env_proxy = getenv("ALL_PROXY");
  }
  if (env_proxy && env_proxy[0]) {
    snprintf(out, cap, "%s", env_proxy);
    return 1;
  }

  const char* ignore_sys = getenv("FV_IGNORE_SYSTEM_PROXY");
  if (ignore_sys && ignore_sys[0] == '1' && ignore_sys[1] == '\0') return 0;

#ifdef __APPLE__
  return fetch_macos_cfnetwork_proxy(scheme, host, out, cap);
#else
  return 0;
#endif
}

static void fv_fetch_apply_system_proxy(CURL* c, const char* url) {
  if (!c || !url || is_loopback_url(url)) return;
  const char* use_sys = getenv("FV_USE_SYSTEM_PROXY");
  if (use_sys && use_sys[0] == '0' && use_sys[1] == '\0') return;
  char proxy_buf[512];
  if (fv_fetch_resolve_system_proxy(url, proxy_buf, sizeof(proxy_buf)) && proxy_buf[0]) {
    curl_easy_setopt(c, CURLOPT_PROXY, proxy_buf);
  }
}

static void setup(CURL* c, const char* url, Buf* b, int creds) {
  set_referer(c, url);
  fv_fetch_apply_system_proxy(c, url);
  if (creds) {
    curl_easy_setopt(c, CURLOPT_SHARE, the_share());
    const char* jar = getenv("FV_COOKIE_JAR");
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, jar ? jar : "");
    if (jar) curl_easy_setopt(c, CURLOPT_COOKIEJAR, jar);
  } else {
    curl_easy_setopt(c, CURLOPT_SHARE, the_bare_share());
  }
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  // a slow answer is waited for as long as it keeps coming (a 1.8 MB script from across the Pacific took more than
  // the old 20 s limit, and its first part ran as if whole); one that stalls for 20 s, or takes 2 minutes, fails
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  // Chrome as sites see it: its TLS and HTTP/2 handshakes and its headers (curl-impersonate); sites behind bot checks
  // turn away a client whose handshake is not a browser's
  curl_easy_impersonate(c, "chrome152", 1);
  if (phone) {
    static struct curl_slist* hs = 0;
    static struct curl_slist* picture_hs = 0;
    if (!hs) hs = phone_headers(0);
    if (!picture_hs) picture_hs = curl_slist_append(phone_headers(0), PICTURE_ACCEPT);
    curl_easy_setopt(c, CURLOPT_USERAGENT, PHONE_UA);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, picture_accept ? picture_hs : hs);
  } else if (picture_accept) {
    static struct curl_slist* picture_hs = 0;
    if (!picture_hs) picture_hs = curl_slist_append(0, PICTURE_ACCEPT);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, picture_hs);
  }
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, take);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
  // FV_CONNECT_TO (curl's HOST:PORT:CONNECT-TO-HOST:PORT, e.g. "::127.0.0.1:"): every request goes to that server
  // instead, which then is trusted as it is; a local test server for WPT's host names
  static struct curl_slist* connect_to = 0;
  const char* to = getenv("FV_CONNECT_TO");
  if (to && *to) {
    if (!connect_to) connect_to = curl_slist_append(0, to);
    curl_easy_setopt(c, CURLOPT_CONNECT_TO, connect_to);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  }
}

moonbit_bytes_t fv_last_url(void) {
  size_t n = strlen(last_url);
  moonbit_bytes_t out = moonbit_make_bytes(n, 0);
  memcpy(out, last_url, n);
  return out;
}

static int is_loopback_url(const char* url) {
  if (!url) return 0;
  const char* p = strstr(url, "://");
  if (!p) return 0;
  p += 3;
  if (strncmp(p, "127.", 4) == 0) return 1;
  if (strncmp(p, "[::1]", 5) == 0) return 1;
  if (strncasecmp(p, "localhost", 9) == 0) {
    char c = p[9];
    if (c == 0 || c == ':' || c == '/' || c == '?' || c == '#') return 1;
  }
  return 0;
}

// Many URLs at once, over parallel connections (a page's pictures, scripts and sheets): the URLs one per line in;
// out, for each in order, its body's length (4 bytes, big-endian) and body, then its final URL's the same way.
moonbit_bytes_t fv_get_many(moonbit_bytes_t list) {
  const char* all = (const char*)list;
  int n = 0;
  for (const char* p = all; *p; p++) if (*p == '\n') n++;
  char** urls = calloc(n + 1, sizeof(char*));
  // a line may be "url<TAB>referer": the page it is fetched for (a frame's document is asked for from its page)
  char** refs = calloc(n + 1, sizeof(char*));
  Buf* bufs = calloc(n + 1, sizeof(Buf));
  CURL** hs = calloc(n + 1, sizeof(CURL*));
  unsigned char* is_lb = calloc(n + 1, 1);
  unsigned char* queued = calloc(n + 1, 1);
  int k = 0;
  for (const char* p = all; *p && k < n;) {
    const char* e = strchr(p, '\n');
    urls[k] = strndup(p, e - p);
    char* tab = strchr(urls[k], '\t');
    if (tab) { *tab = 0; refs[k] = strdup(tab + 1); }
    p = e + 1;
    k++;
  }
  CURLM* m = curl_multi_init();
  // Never set CURLMOPT_MAX_HOST_CONNECTIONS on a multi whose handles share CURLOPT_SHARE(the_share()):
  // libcurl can deadlock queued HTTP/2 handles until the 120s timeout (e.g. github.com).
  // Instead, throttle only loopback (127.0.0.1 / localhost / [::1]) with an explicit 6-in-flight window
  // so local test servers (python3 http.server listen(5) backlog) never drop connections.
  int lb_in_flight = 0, queued_left = 0;
  for (int i = 0; i < n; i++) {
    hs[i] = curl_easy_init();
    setup(hs[i], urls[i], &bufs[i], 1);
    if (refs[i]) curl_easy_setopt(hs[i], CURLOPT_REFERER, refs[i]);
    is_lb[i] = (unsigned char)is_loopback_url(urls[i]);
    if (!is_lb[i] || lb_in_flight < 6) {
      curl_multi_add_handle(m, hs[i]);
      if (is_lb[i]) lb_in_flight++;
    } else {
      queued[i] = 1;
      queued_left++;
    }
  }
  int running = 1;
  while (running || queued_left > 0) {
    curl_multi_perform(m, &running);
    int added = 0;
    CURLMsg* msg; int left;
    while ((msg = curl_multi_info_read(m, &left))) {
      if (msg->msg != CURLMSG_DONE) continue;
      for (int i = 0; i < n; i++) if (hs[i] == msg->easy_handle) {
        if (msg->data.result != CURLE_OK) {
          if (getenv("FV_FETCH_LOG")) fprintf(stderr, "fetch failed (%s) %s\n", curl_easy_strerror(msg->data.result), urls[i]);
          bufs[i].n = 0;
        }
        if (is_lb[i]) {
          if (lb_in_flight > 0) lb_in_flight--;
          if (queued_left > 0 && lb_in_flight < 6) {
            for (int j = 0; j < n; j++) if (queued[j]) {
              queued[j] = 0;
              queued_left--;
              curl_multi_add_handle(m, hs[j]);
              lb_in_flight++;
              added = 1;
              break;
            }
          }
        }
        break;
      }
    }
    if (!added && running) curl_multi_poll(m, 0, 0, 1000, 0);
  }
  size_t total = 0;
  char** finals = calloc(n + 1, sizeof(char*));
  for (int i = 0; i < n; i++) {
    char* u = 0;
    curl_easy_getinfo(hs[i], CURLINFO_EFFECTIVE_URL, &u);
    finals[i] = strdup(u ? u : urls[i]);
    if (getenv("FV_FETCH_LOG")) {
      double t = 0;
      curl_easy_getinfo(hs[i], CURLINFO_TOTAL_TIME, &t);
      fprintf(stderr, "fetch %.2fs %zu %s\n", t, bufs[i].n, urls[i]);
    }
    total += 8 + bufs[i].n + strlen(finals[i]);
  }
  moonbit_bytes_t out = moonbit_make_bytes(total, 0);
  unsigned char* o = (unsigned char*)out;
  for (int i = 0; i < n; i++) {
    size_t parts[2] = {bufs[i].n, strlen(finals[i])};
    const char* data[2] = {bufs[i].p, finals[i]};
    for (int j = 0; j < 2; j++) {
      size_t len = parts[j];
      *o++ = len >> 24; *o++ = len >> 16; *o++ = len >> 8; *o++ = len;
      if (len) memcpy(o, data[j], len);
      o += len;
    }
    curl_multi_remove_handle(m, hs[i]);
    curl_easy_cleanup(hs[i]);
    free(bufs[i].p);
    free(urls[i]);
    free(refs[i]);
    free(finals[i]);
  }
  curl_multi_cleanup(m);
  free(urls); free(refs); free(bufs); free(hs); free(is_lb); free(queued); free(finals);
  return out;
}

static size_t take_header(char* d, size_t s, size_t m, void* u) { return take(d, s, m, u); }

// A request as a page's script makes it (XMLHttpRequest, fetch, a form sent): method, URL, headers ("Name: value"
// lines), body. Out: status (4 bytes), then the response's headers, final URL and body, each with its length (4 bytes).
// A request's handle made ready: method, URL, headers ("Name: value" lines), body; its answer into b, headers into h.
// The header list it returns is freed when the request is done.
static struct curl_slist* prepare(CURL* c, const char* m, const char* url, const char* headers, const char* body, int32_t body_n, Buf* b, Buf* h, int creds) {
  setup(c, url, b, creds);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, take_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, h);
  struct curl_slist* hs = phone ? phone_headers(0) : 0;
  for (const char* p = headers; *p;) {
    const char* e = strchr(p, '\n');
    if (!e) e = p + strlen(p);
    char* line = strndup(p, e - p);
    if (*line) hs = curl_slist_append(hs, line);
    free(line);
    p = *e ? e + 1 : e;
  }
  if (hs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  if (strcmp(m, "GET") != 0) {
    if (strcmp(m, "POST") == 0) curl_easy_setopt(c, CURLOPT_POST, 1L);
    else curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, m);
    if (strcmp(m, "HEAD") == 0) curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body_n);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body);
  }
  return hs;
}

// A done request's answer: status (4 bytes), then the last response's headers, final URL and body, each with its
// length (4 bytes); `with_body` 0 leaves the body's length out (it runs to the end).
static size_t last_headers(Buf* h) {
  size_t hstart = 0;
  for (size_t i = 0; i + 7 <= h->n; i++) if (memcmp(h->p + i, "\r\nHTTP/", 7) == 0) hstart = i + 2;
  return hstart;
}
static size_t answer_size(Buf* h, const char* fin, Buf* b) {
  return 4 + 4 + (h->n - last_headers(h)) + 4 + (fin ? strlen(fin) : 0) + 4 + b->n;
}
static unsigned char* put_answer(unsigned char* o, long status, Buf* h, const char* fin, Buf* b, int with_body) {
  #define PUT32(v) do { size_t _v = (v); *o++ = _v >> 24; *o++ = _v >> 16; *o++ = _v >> 8; *o++ = _v; } while (0)
  // only the last response's headers (after redirects)
  size_t hstart = last_headers(h);
  size_t hn = h->n - hstart, fn = fin ? strlen(fin) : 0;
  PUT32(status);
  PUT32(hn); if (hn) memcpy(o, h->p + hstart, hn); o += hn;
  PUT32(fn); if (fn) memcpy(o, fin, fn); o += fn;
  if (with_body) PUT32(b->n);
  if (b->n) memcpy(o, b->p, b->n);
  o += b->n;
  return o;
}

// A request as a page's script makes it (XMLHttpRequest, fetch, a form sent): method, URL, headers ("Name: value"
// lines), body. Out: status (4 bytes), then the response's headers, final URL and body, each with its length (4 bytes).
// (creds 0: no cookies go with it or are kept from it)
moonbit_bytes_t fv_request(moonbit_bytes_t method, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body, int32_t body_n, int32_t creds) {
  Buf b = {0, 0}, h = {0, 0};
  CURL* c = curl_easy_init();
  struct curl_slist* hs = prepare(c, (const char*)method, (const char*)url, (const char*)headers, (const char*)body, body_n, &b, &h, creds);
  long status = 0;
  char* fin = 0;
  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &fin);
  } else if (getenv("FV_FETCH_LOG")) fprintf(stderr, "request failed (%s) %s\n", curl_easy_strerror(rc), (const char*)url);
  moonbit_bytes_t out = moonbit_make_bytes(answer_size(&h, fin, &b) - 4, 0);
  put_answer((unsigned char*)out, status, &h, fin, &b, 0);
  curl_slist_free_all(hs);
  curl_easy_cleanup(c);
  free(b.p); free(h.p);
  return out;
}

// Requests a page's scripts made that go on while its scripts run (a script's src, an XMLHttpRequest, a fetch): each
// started, then taken when done, as a browser's network answers a page later.
typedef struct { int32_t id; CURL* c; Buf b, h; struct curl_slist* hs; int is_lb; int added; } Pending;
static CURLM* amulti = 0;
// (each its own allocation: curl writes into its buffers by address while the list changes)
static Pending** pend = 0;
static int npend = 0, cappend = 0;
static int async_lb_in_flight = 0;
static int32_t next_id = 1;

static void async_admit_loopback(void) {
  if (!amulti) return;
  int added = 0;
  for (int i = 0; i < npend && async_lb_in_flight < 6; i++) {
    if (pend[i]->is_lb && !pend[i]->added) {
      curl_multi_add_handle(amulti, pend[i]->c);
      pend[i]->added = 1;
      async_lb_in_flight++;
      added = 1;
    }
  }
  if (added) {
    int running = 0;
    curl_multi_perform(amulti, &running);
  }
}

int32_t fv_async_start(moonbit_bytes_t method, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body, int32_t body_n, int32_t creds) {
  if (!amulti) amulti = curl_multi_init();
  if (npend == cappend) { cappend = cappend ? cappend * 2 : 16; pend = realloc(pend, cappend * sizeof(Pending*)); }
  Pending* p = calloc(1, sizeof(Pending));
  pend[npend++] = p;
  p->id = next_id++;
  p->c = curl_easy_init();
  p->hs = prepare(p->c, (const char*)method, (const char*)url, (const char*)headers, (const char*)body, body_n, &p->b, &p->h, creds);
  p->is_lb = is_loopback_url((const char*)url);
  curl_easy_setopt(p->c, CURLOPT_PRIVATE, (void*)(intptr_t)p->id);
  if (!p->is_lb || async_lb_in_flight < 6) {
    curl_multi_add_handle(amulti, p->c);
    p->added = 1;
    if (p->is_lb) async_lb_in_flight++;
    int running = 0;
    curl_multi_perform(amulti, &running);
  }
  return p->id;
}

int32_t fv_async_pending(void) { return npend; }

// The requests done (waiting up to wait_ms for one when none is): for each, its id (4 bytes) and its answer as
// fv_request gives it, the body's length given too.
moonbit_bytes_t fv_async_poll(int32_t wait_ms) {
  if (!amulti || npend == 0) return moonbit_make_bytes(0, 0);
  int running = 0;
  int32_t done[256]; long st[256]; int nd = 0;
  // (when nothing is done yet, waited for until something is or wait_ms of real time has gone: curl_multi_poll can
  // come back at once with nothing done, while transfers queued for a connection wait on others, and a page with
  // more requests out than connections was then taken as settled in a few milliseconds with answers still to come)
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int round = 0; nd == 0; round++) {
    if (round > 0) {
      struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      long gone = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
      if (wait_ms <= 0) {
        // Drain already-ready kernel socket buffers without blocking (up to 8 ms) so multi-MB async fetches
        // (e.g. <video> / large assets) run at wire speed instead of 1 TLS record per host tick.
        if (gone >= 8 || !running) break;
        int numfds = 0;
        curl_multi_poll(amulti, 0, 0, 0, &numfds);
        if (numfds <= 0) break;
      } else {
        if (gone >= wait_ms) break;
        curl_multi_poll(amulti, 0, 0, (int)(wait_ms - gone), 0);
      }
    }
    curl_multi_perform(amulti, &running);
    CURLMsg* msg; int left;
    while (nd < 256 && (msg = curl_multi_info_read(amulti, &left))) {
      if (msg->msg != CURLMSG_DONE) continue;
      void* priv = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
      long status = 0;
      if (msg->data.result == CURLE_OK) curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status);
      else {
        if (getenv("FV_FETCH_LOG")) { char* u = 0; curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &u); fprintf(stderr, "async failed (%s) %s\n", curl_easy_strerror(msg->data.result), u ? u : "?"); }
        for (int i = 0; i < npend; i++) if (pend[i]->c == msg->easy_handle) pend[i]->b.n = 0;
      }
      done[nd] = (int32_t)(intptr_t)priv; st[nd] = status; nd++;
    }
  }
  size_t total = 0;
  for (int k = 0; k < nd; k++) for (int i = 0; i < npend; i++) if (pend[i]->id == done[k]) {
    char* fin = 0; curl_easy_getinfo(pend[i]->c, CURLINFO_EFFECTIVE_URL, &fin);
    total += 4 + answer_size(&pend[i]->h, fin, &pend[i]->b);
  }
  moonbit_bytes_t out = moonbit_make_bytes(total, 0);
  unsigned char* o = (unsigned char*)out;
  for (int k = 0; k < nd; k++) for (int i = 0; i < npend; i++) if (pend[i]->id == done[k]) {
    Pending* p = pend[i];
    char* fin = 0; curl_easy_getinfo(p->c, CURLINFO_EFFECTIVE_URL, &fin);
    PUT32(p->id);
    o = put_answer(o, st[k], &p->h, fin, &p->b, 1);
    curl_multi_remove_handle(amulti, p->c);
    if (p->is_lb && p->added && async_lb_in_flight > 0) async_lb_in_flight--;
    curl_easy_cleanup(p->c);
    curl_slist_free_all(p->hs);
    free(p->b.p); free(p->h.p);
    free(p);
    pend[i] = pend[--npend];
    break;
  }
  async_admit_loopback();
  return out;
}

// Requests not wanted any more (a new page): dropped.
void fv_async_clear(void) {
  for (int i = 0; i < npend; i++) {
    if (pend[i]->added) curl_multi_remove_handle(amulti, pend[i]->c);
    curl_easy_cleanup(pend[i]->c);
    curl_slist_free_all(pend[i]->hs);
    free(pend[i]->b.p); free(pend[i]->h.p);
    free(pend[i]);
  }
  npend = 0;
  async_lb_in_flight = 0;
}

// Streaming HTTP requests (fetch().body ReadableStream, EventSource SSE): delivers HEAD, CHUNK and DONE events
// incrementally as bytes arrive instead of buffering the whole response body in memory.
typedef struct {
  int32_t id;
  CURL* c;
  Buf h;
  struct curl_slist* hs;
  int head_sent;
  int aborted;
  int is_lb;
  int added;
} StreamReq;

static CURLM* smulti = 0;
static StreamReq** streams = 0;
static int nstream = 0, capstream = 0;
static int stream_lb_in_flight = 0;
static Buf stream_ev = {0, 0};

static void stream_admit_loopback(void) {
  if (!smulti) return;
  int added = 0;
  for (int i = 0; i < nstream && stream_lb_in_flight < 6; i++) {
    if (streams[i]->is_lb && !streams[i]->added && !streams[i]->aborted) {
      curl_multi_add_handle(smulti, streams[i]->c);
      streams[i]->added = 1;
      stream_lb_in_flight++;
      added = 1;
    }
  }
  if (added) {
    int running = 0;
    curl_multi_perform(smulti, &running);
  }
}

static void sev_u8(unsigned char v) {
  take((char*)&v, 1, 1, &stream_ev);
}
static void sev_u32(size_t v) {
  unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
  take((char*)b, 1, 4, &stream_ev);
}
static void sev_bytes(const char* p, size_t n) {
  sev_u32(n);
  if (n) take((char*)p, 1, n, &stream_ev);
}

static void emit_stream_head(StreamReq* p, long status, const char* fin) {
  if (p->head_sent) return;
  p->head_sent = 1;
  size_t hstart = last_headers(&p->h);
  size_t hn = p->h.n - hstart;
  size_t fn = fin ? strlen(fin) : 0;
  sev_u8(1); // HEAD
  sev_u32((size_t)(uint32_t)p->id);
  sev_u32((size_t)(uint32_t)status);
  sev_bytes(p->h.p + hstart, hn);
  sev_bytes(fin ? fin : "", fn);
}

static size_t take_stream_header(char* d, size_t s, size_t m, void* u) {
  StreamReq* p = (StreamReq*)u;
  if (p->aborted) return 0;
  size_t k = take(d, s, m, &p->h);
  // End of a response header block: if this is not a 1xx informational or 3xx redirect response, emit HEAD now
  // so SSE / slow-first-chunk streams resolve fetch() and open EventSource as soon as headers arrive.
  if (!p->head_sent && ((k == 2 && d[0] == '\r' && d[1] == '\n') || (k == 1 && d[0] == '\n'))) {
    long status = 0;
    curl_easy_getinfo(p->c, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 200 && (status < 300 || status >= 400)) {
      char* fin = 0;
      curl_easy_getinfo(p->c, CURLINFO_EFFECTIVE_URL, &fin);
      emit_stream_head(p, status, fin);
    }
  }
  return k;
}

static size_t take_stream_body(char* d, size_t s, size_t m, void* u) {
  StreamReq* p = (StreamReq*)u;
  if (p->aborted) return 0;
  size_t k = s * m;
  if (!p->head_sent) {
    long status = 0;
    char* fin = 0;
    curl_easy_getinfo(p->c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(p->c, CURLINFO_EFFECTIVE_URL, &fin);
    emit_stream_head(p, status, fin);
  }
  if (k > 0) {
    sev_u8(2); // CHUNK
    sev_u32((size_t)(uint32_t)p->id);
    sev_bytes(d, k);
  }
  return k;
}

int32_t fv_stream_start(moonbit_bytes_t method, moonbit_bytes_t url, moonbit_bytes_t headers, moonbit_bytes_t body, int32_t body_n, int32_t creds, int32_t long_lived) {
  if (!smulti) smulti = curl_multi_init();
  if (nstream == capstream) {
    capstream = capstream ? capstream * 2 : 16;
    streams = realloc(streams, capstream * sizeof(StreamReq*));
  }
  StreamReq* p = calloc(1, sizeof(StreamReq));
  streams[nstream++] = p;
  p->id = next_id++;
  p->c = curl_easy_init();
  Buf dummy = {0, 0};
  p->hs = prepare(p->c, (const char*)method, (const char*)url, (const char*)headers, (const char*)body, body_n, &dummy, &p->h, creds);
  curl_easy_setopt(p->c, CURLOPT_WRITEFUNCTION, take_stream_body);
  curl_easy_setopt(p->c, CURLOPT_WRITEDATA, p);
  curl_easy_setopt(p->c, CURLOPT_HEADERFUNCTION, take_stream_header);
  curl_easy_setopt(p->c, CURLOPT_HEADERDATA, p);
  if (long_lived) {
    // Long-lived streams (SSE / EventSource) may pause indefinitely between events.
    curl_easy_setopt(p->c, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(p->c, CURLOPT_LOW_SPEED_TIME, 0L);
    curl_easy_setopt(p->c, CURLOPT_TIMEOUT, 0L);
  }
  curl_easy_setopt(p->c, CURLOPT_PRIVATE, (void*)(intptr_t)p->id);
  p->is_lb = is_loopback_url((const char*)url);
  if (!p->is_lb || stream_lb_in_flight < 6) {
    curl_multi_add_handle(smulti, p->c);
    p->added = 1;
    if (p->is_lb) stream_lb_in_flight++;
    int running = 0;
    curl_multi_perform(smulti, &running);
  }
  return p->id;
}

static void free_stream_at(int idx) {
  StreamReq* p = streams[idx];
  if (p->added) {
    curl_multi_remove_handle(smulti, p->c);
    if (p->is_lb && stream_lb_in_flight > 0) stream_lb_in_flight--;
  }
  curl_easy_cleanup(p->c);
  curl_slist_free_all(p->hs);
  free(p->h.p);
  free(p);
  streams[idx] = streams[--nstream];
  stream_admit_loopback();
}

void fv_stream_abort(int32_t id) {
  for (int i = 0; i < nstream; i++) {
    if (streams[i]->id == id) {
      streams[i]->aborted = 1;
      free_stream_at(i);
      return;
    }
  }
}

int32_t fv_stream_active(void) { return nstream; }

void fv_stream_clear(void) {
  for (int i = 0; i < nstream; i++) {
    streams[i]->aborted = 1;
  }
  while (nstream > 0) {
    free_stream_at(0);
  }
  stream_lb_in_flight = 0;
  free(stream_ev.p);
  stream_ev.p = 0;
  stream_ev.n = 0;
}

moonbit_bytes_t fv_stream_poll(int32_t wait_ms) {
  if (smulti && nstream > 0) {
    int running = 0;
    struct timespec t0;
    if (wait_ms > 0) clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
      curl_multi_perform(smulti, &running);
      CURLMsg* msg;
      int left = 0;
      while ((msg = curl_multi_info_read(smulti, &left))) {
        if (msg->msg != CURLMSG_DONE) continue;
        void* priv = 0;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
        int32_t id = (int32_t)(intptr_t)priv;
        int idx = -1;
        for (int i = 0; i < nstream; i++) if (streams[i]->id == id) { idx = i; break; }
        if (idx < 0) continue;
        StreamReq* p = streams[idx];
        CURLcode rc = msg->data.result;
        if (!p->aborted) {
          if (!p->head_sent) {
            long status = 0;
            char* fin = 0;
            if (rc == CURLE_OK) {
              curl_easy_getinfo(p->c, CURLINFO_RESPONSE_CODE, &status);
              curl_easy_getinfo(p->c, CURLINFO_EFFECTIVE_URL, &fin);
            }
            emit_stream_head(p, status, fin);
          }
          const char* err = (rc == CURLE_OK) ? "" : curl_easy_strerror(rc);
          sev_u8(3); // DONE
          sev_u32((size_t)(uint32_t)p->id);
          sev_u8(rc == CURLE_OK ? 1 : 0);
          sev_bytes(err, strlen(err));
        }
        free_stream_at(idx);
      }
      if (stream_ev.n > 0 || wait_ms <= 0 || running <= 0 || nstream <= 0) break;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long gone = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
      if (gone >= wait_ms) break;
      curl_multi_poll(smulti, 0, 0, (int)(wait_ms - gone), 0);
    }
  }
  moonbit_bytes_t out = moonbit_make_bytes(stream_ev.n, 0);
  if (stream_ev.n) memcpy(out, stream_ev.p, stream_ev.n);
  stream_ev.n = 0;
  return out;
}

// The cookie jar's lines (Netscape format, "#HttpOnly_" before a script-hidden cookie's domain).
moonbit_bytes_t fv_cookies(void) {
  CURL* c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_SHARE, the_share());
  curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
  struct curl_slist* list = 0;
  curl_easy_getinfo(c, CURLINFO_COOKIELIST, &list);
  Buf b = {0, 0};
  for (struct curl_slist* l = list; l; l = l->next) { take(l->data, 1, strlen(l->data), &b); take("\n", 1, 1, &b); }
  curl_slist_free_all(list);
  curl_easy_cleanup(c);
  moonbit_bytes_t out = moonbit_make_bytes(b.n, 0);
  if (b.n) memcpy(out, b.p, b.n);
  free(b.p);
  return out;
}

// A cookie a script sets: a Set-Cookie line (with its domain made explicit) into the jar.
void fv_set_cookie(moonbit_bytes_t line) {
  CURL* c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_SHARE, the_share());
  curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(c, CURLOPT_COOKIELIST, (const char*)line);
  curl_easy_cleanup(c);
}

// A picture in a format the client does not decode (lossy WebP, AVIF, HEIC, TIFF...) made a PNG by the system's
// decoders (ImageIO); empty when it cannot be. Elsewhere than macOS none of the system's decoders is asked: empty, and
// a size of 0 (unknown).
#ifdef __APPLE__
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>

moonbit_bytes_t fv_to_png(moonbit_bytes_t data, int32_t n) {
  moonbit_bytes_t out = moonbit_make_bytes(0, 0);
  CFDataRef in = CFDataCreate(NULL, (const UInt8*)data, n);
  if (!in) return out;
  CGImageSourceRef src = CGImageSourceCreateWithData(in, NULL);
  CFRelease(in);
  if (!src) return out;
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!img) return out;
  CFMutableDataRef png = CFDataCreateMutable(NULL, 0);
  CGImageDestinationRef dst = CGImageDestinationCreateWithData(png, CFSTR("public.png"), 1, NULL);
  if (dst) {
    CGImageDestinationAddImage(dst, img, NULL);
    if (CGImageDestinationFinalize(dst)) {
      CFIndex len = CFDataGetLength(png);
      out = moonbit_make_bytes((int32_t)len, 0);
      memcpy(out, CFDataGetBytePtr(png), (size_t)len);
    }
    CFRelease(dst);
  }
  CFRelease(png);
  CGImageRelease(img);
  return out;
}

// A picture made no bigger than `max_px` on its longer side (when it is bigger), as JPEG when it is opaque and PNG
// when it is not; empty when ImageIO cannot read it, or the result is no smaller and `any_size` is 0.
moonbit_bytes_t fv_shrink(moonbit_bytes_t data, int32_t n, int32_t max_px, int32_t any_size) {
  moonbit_bytes_t out = moonbit_make_bytes(0, 0);
  CFDataRef in = CFDataCreate(NULL, (const UInt8*)data, n);
  if (!in) return out;
  CGImageSourceRef src = CGImageSourceCreateWithData(in, NULL);
  CFRelease(in);
  if (!src) return out;
  CFNumberRef mp = CFNumberCreate(NULL, kCFNumberSInt32Type, &max_px);
  const void* k[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceCreateThumbnailWithTransform};
  const void* v[] = {kCFBooleanTrue, mp, kCFBooleanTrue};
  CFDictionaryRef opts = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
  CFRelease(opts);
  CFRelease(mp);
  CFRelease(src);
  if (!img) return out;
  CGImageAlphaInfo a = CGImageGetAlphaInfo(img);
  int opaque = a == kCGImageAlphaNone || a == kCGImageAlphaNoneSkipFirst || a == kCGImageAlphaNoneSkipLast;
  CFMutableDataRef enc = CFDataCreateMutable(NULL, 0);
  CGImageDestinationRef dst = CGImageDestinationCreateWithData(enc, opaque ? CFSTR("public.jpeg") : CFSTR("public.png"), 1, NULL);
  if (dst) {
    float q = 0.82f;
    CFNumberRef qn = CFNumberCreate(NULL, kCFNumberFloatType, &q);
    const void* dk[] = {kCGImageDestinationLossyCompressionQuality};
    const void* dv[] = {qn};
    CFDictionaryRef props = CFDictionaryCreate(NULL, dk, dv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CGImageDestinationAddImage(dst, img, props);
    CFRelease(props);
    CFRelease(qn);
    if (CGImageDestinationFinalize(dst)) {
      CFIndex len = CFDataGetLength(enc);
      if (len > 0 && (any_size || len < n)) {
        out = moonbit_make_bytes((int32_t)len, 0);
        memcpy(out, CFDataGetBytePtr(enc), (size_t)len);
      }
    }
    CFRelease(dst);
  }
  CFRelease(enc);
  CGImageRelease(img);
  return out;
}

// A picture's pixel size: (width << 16) | height, 0 when ImageIO cannot read it.
int32_t fv_picture_size(moonbit_bytes_t data, int32_t n) {
  CFDataRef in = CFDataCreate(NULL, (const UInt8*)data, n);
  if (!in) return 0;
  CGImageSourceRef src = CGImageSourceCreateWithData(in, NULL);
  CFRelease(in);
  if (!src) return 0;
  CFDictionaryRef p = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
  CFRelease(src);
  if (!p) return 0;
  int w = 0, h = 0;
  CFNumberRef wn = CFDictionaryGetValue(p, kCGImagePropertyPixelWidth), hn = CFDictionaryGetValue(p, kCGImagePropertyPixelHeight);
  if (wn) CFNumberGetValue(wn, kCFNumberIntType, &w);
  if (hn) CFNumberGetValue(hn, kCFNumberIntType, &h);
  CFRelease(p);
  if (w > 32767 || h > 65535) return 0;
  return (w << 16) | h;
}

#else
moonbit_bytes_t fv_to_png(moonbit_bytes_t data, int32_t n) {
  (void)data;
  (void)n;
  return moonbit_make_bytes(0, 0);
}
moonbit_bytes_t fv_shrink(moonbit_bytes_t data, int32_t n, int32_t max_px, int32_t any_size) {
  (void)data;
  (void)n;
  (void)max_px;
  (void)any_size;
  return moonbit_make_bytes(0, 0);
}
int32_t fv_picture_size(moonbit_bytes_t data, int32_t n) {
  (void)data;
  (void)n;
  return 0;
}
#endif

// The process's resident memory in MB (for traces).
#ifdef __APPLE__
#include <mach/mach.h>
int32_t fv_rss_mb(void) {
  struct mach_task_basic_info info;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return 0;
  return (int32_t)(info.resident_size >> 20);
}
#elif defined(_WIN32)
int32_t fv_rss_mb(void) {
  PROCESS_MEMORY_COUNTERS pmc;
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return (int32_t)(pmc.WorkingSetSize >> 20);
}
#else
#include <unistd.h>
int32_t fv_rss_mb(void) {
  // (Linux's: resident pages, the second of /proc/self/statm)
  FILE* f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long size = 0, resident = 0;
  int got = fscanf(f, "%ld %ld", &size, &resident);
  fclose(f);
  if (got != 2) return 0;
  return (int32_t)((resident * sysconf(_SC_PAGESIZE)) >> 20);
}
#endif

// A line of FV_PROF's account, unbuffered (on stderr: a host ended by a signal loses nothing of it).
void fv_prof_line(moonbit_bytes_t s) { fprintf(stderr, "%s\n", (const char*)s); }

// FV_SAMPLE=<file>: a sampling profiler for looking into where the host's time goes (macOS's `sample` cannot attach on
// every machine): every millisecond the main thread is running, another thread stops it, reads its registers and walks
// its frame pointers. When the program ends (SIGTERM, or by itself) the stacks go to the file, one per line as
// addresses, after the image's slide (for `atos -s`). (A SIGPROF timer's sample lands where the thread next comes
// back from the kernel: page faults and system calls took a fifth of its samples, an allocator's fresh pages and a
// thread pool's wake-ups looked like the hottest code there was.)
#ifndef _WIN32
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif

#define SAMPLE_DEPTH 48
#define SAMPLE_MAX 200000
#ifdef __APPLE__
static void** samples = 0;
static unsigned char* sample_depths = 0;
static volatile int nsamples = 0;
static const char* sample_path = 0;
static volatile int sampler_stop = 0;
static volatile int sampler_done = 0;
static mach_port_t sampled_thread = MACH_PORT_NULL;
#endif

// FV_SAMPLE_WINDOWED: only the samples within a window after each fv_sample_window call (a tap's answer, apart from
// the page's load); -1: every sample
static volatile long long window_until = -1;
static long long sample_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// (FV_PROF) microseconds of a monotonic clock, for timing what takes less than a millisecond
int64_t fv_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
void fv_sample_window(int32_t ms) {
  if (window_until >= 0) window_until = sample_now_ms() + ms;
}

// (the sampler is macOS's, its threads' state read through Mach: elsewhere FV_SAMPLE writes nothing)
#ifdef __APPLE__
// One sample of the stopped thread: its pc, then the return address of each frame, leaf first.
static int take_stack(void** out) {
  int d = 0;
#if defined(__arm64__)
  arm_thread_state64_t st;
  mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
  if (thread_get_state(sampled_thread, ARM_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS) return 0;
  out[d++] = (void*)arm_thread_state64_get_pc(st);
  uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
  // (the caller of a function that keeps no frame of its own, as the runtime's drop and alloc: only in the link
  // register; where it is the frame's own return address already, it is not listed twice)
  uintptr_t lr = (uintptr_t)arm_thread_state64_get_lr(st) & 0x0000ffffffffffffULL;
  if (lr && !(fp && (fp & 7) == 0 && (((uintptr_t*)fp)[1] & 0x0000ffffffffffffULL) == lr)) out[d++] = (void*)lr;
#elif defined(__x86_64__)
  x86_thread_state64_t st;
  mach_msg_type_number_t c = x86_THREAD_STATE64_COUNT;
  if (thread_get_state(sampled_thread, x86_THREAD_STATE64, (thread_state_t)&st, &c) != KERN_SUCCESS) return 0;
  out[d++] = (void*)st.__rip;
  uintptr_t fp = (uintptr_t)st.__rbp;
#endif
  while (d < SAMPLE_DEPTH && fp && (fp & 7) == 0) {
    uintptr_t* f = (uintptr_t*)fp;
    uintptr_t next = f[0], ret = f[1] & 0x0000ffffffffffffULL;  // (a return address without its pointer signature)
    if (!ret) break;
    out[d++] = (void*)ret;
    if (next <= fp) break;
    fp = next;
  }
  return d;
}

static void* sampler_main(void* arg) {
  (void)arg;
  while (!sampler_stop && nsamples < SAMPLE_MAX) {
    usleep(1000);
    if (window_until >= 0 && sample_now_ms() > window_until) continue;
    struct thread_basic_info info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(sampled_thread, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) break;
    // (a thread waiting, on the network or a timer, is not where time goes)
    if (info.run_state != TH_STATE_RUNNING) continue;
    if (thread_suspend(sampled_thread) != KERN_SUCCESS) break;
    int i = nsamples;
    sample_depths[i] = (unsigned char)take_stack(samples + (size_t)i * SAMPLE_DEPTH);
    thread_resume(sampled_thread);
    if (sample_depths[i] > 0) nsamples = i + 1;
  }
  sampler_done = 1;
  return 0;
}

static void write_samples(void) {
  sampler_stop = 1;
  for (int i = 0; i < 100 && !sampler_done; i++) usleep(1000);
  FILE* f = fopen(sample_path, "w");
  if (f) {
    // (and where libnode, V8's image, was loaded, for `atos -o <it> -l`)
    fprintf(f, "slide %lx", (unsigned long)_dyld_get_image_vmaddr_slide(0));
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
      const char* name = _dyld_get_image_name(i);
      if (name && strstr(name, "libnode")) fprintf(f, " %s %lx", name, (unsigned long)_dyld_get_image_header(i));
    }
    fputc('\n', f);
    for (int i = 0; i < nsamples; i++) {
      for (int k = 0; k < sample_depths[i]; k++) fprintf(f, "%lx ", (unsigned long)samples[(size_t)i * SAMPLE_DEPTH + k]);
      fputc('\n', f);
    }
    fclose(f);
  }
}

void fv_sample_flush(void) {
  if (sample_path && samples) write_samples();
}

static void dump_samples(int sig) {
  write_samples();
  _exit(sig == SIGTERM ? 0 : 1);
}

// A forked child (the host's page process for each connection) has no sampler thread: it starts its own, and its
// samples go to <file>.<pid>.
static char child_path[1024];
static void sample_child(void) {
  snprintf(child_path, sizeof child_path, "%s.%d", getenv("FV_SAMPLE"), (int)getpid());
  sample_path = child_path;
  nsamples = 0;
  sampler_stop = 0;
  sampler_done = 0;
  sampled_thread = mach_thread_self();
  pthread_t t;
  pthread_create(&t, 0, sampler_main, 0);
}

__attribute__((constructor)) static void start_sampling(void) {
  sample_path = getenv("FV_SAMPLE");
  if (!sample_path || !*sample_path) return;
  if (getenv("FV_SAMPLE_WINDOWED")) window_until = 0;
  samples = calloc((size_t)SAMPLE_MAX * SAMPLE_DEPTH, sizeof(void*));
  sample_depths = calloc(SAMPLE_MAX, 1);
  signal(SIGTERM, dump_samples);
  // (a program that ends by itself, wpt.exe or a layout, writes them too)
  atexit(write_samples);
  // (the constructor runs on the main thread: it is the one sampled)
  sampled_thread = mach_thread_self();
  pthread_t t;
  pthread_create(&t, 0, sampler_main, 0);
  pthread_atfork(0, 0, sample_child);
}
#else
void fv_sample_flush(void) {}
#endif

// The last get's Content-Disposition ("" where it had none).
moonbit_bytes_t fv_last_disposition(void) {
  size_t n = strlen(last_disposition);
  moonbit_bytes_t out = moonbit_make_bytes(n, 0);
  memcpy(out, last_disposition, n);
  return out;
}

// The last get's Content-Type (its charset says how a page's bytes are text).
moonbit_bytes_t fv_last_content_type(void) {
  size_t n = strlen(last_ctype);
  moonbit_bytes_t out = moonbit_make_bytes(n, 0);
  memcpy(out, last_ctype, n);
  return out;
}

// Bytes in a legacy encoding (GBK, Big5, Shift_JIS, EUC-KR, windows-1252...) as UTF-8, by iconv; what does not
// convert becomes U+FFFD, as browsers decode. An empty result: iconv does not know the encoding.
#ifdef _WIN32
static UINT fv_win_codepage(const char* cs) {
  if (!cs || !cs[0]) return 0;
  if (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "UTF8") == 0) return CP_UTF8;
  if (strcasecmp(cs, "GBK") == 0 || strcasecmp(cs, "GB2312") == 0 || strcasecmp(cs, "CP936") == 0) return 936;
  if (strcasecmp(cs, "GB18030") == 0) return 54936;
  if (strcasecmp(cs, "BIG5") == 0 || strcasecmp(cs, "BIG5-HKSCS") == 0 || strcasecmp(cs, "CP950") == 0) return 950;
  if (strcasecmp(cs, "SHIFT_JIS") == 0 || strcasecmp(cs, "SHIFT-JIS") == 0 || strcasecmp(cs, "SJIS") == 0 || strcasecmp(cs, "CP932") == 0) return 932;
  if (strcasecmp(cs, "EUC-JP") == 0) return 20932;
  if (strcasecmp(cs, "ISO-2022-JP") == 0) return 50220;
  if (strcasecmp(cs, "EUC-KR") == 0 || strcasecmp(cs, "CP949") == 0) return 949;
  if (strcasecmp(cs, "WINDOWS-874") == 0 || strcasecmp(cs, "CP874") == 0) return 874;
  if (strcasecmp(cs, "WINDOWS-1250") == 0 || strcasecmp(cs, "CP1250") == 0) return 1250;
  if (strcasecmp(cs, "WINDOWS-1251") == 0 || strcasecmp(cs, "CP1251") == 0) return 1251;
  if (strcasecmp(cs, "WINDOWS-1252") == 0 || strcasecmp(cs, "CP1252") == 0) return 1252;
  if (strcasecmp(cs, "WINDOWS-1253") == 0 || strcasecmp(cs, "CP1253") == 0) return 1253;
  if (strcasecmp(cs, "WINDOWS-1254") == 0 || strcasecmp(cs, "CP1254") == 0) return 1254;
  if (strcasecmp(cs, "WINDOWS-1255") == 0 || strcasecmp(cs, "CP1255") == 0) return 1255;
  if (strcasecmp(cs, "WINDOWS-1256") == 0 || strcasecmp(cs, "CP1256") == 0) return 1256;
  if (strcasecmp(cs, "WINDOWS-1257") == 0 || strcasecmp(cs, "CP1257") == 0) return 1257;
  if (strcasecmp(cs, "WINDOWS-1258") == 0 || strcasecmp(cs, "CP1258") == 0) return 1258;
  if (strcasecmp(cs, "ISO-8859-1") == 0) return 28591;
  if (strcasecmp(cs, "ISO-8859-2") == 0) return 28592;
  if (strcasecmp(cs, "ISO-8859-3") == 0) return 28593;
  if (strcasecmp(cs, "ISO-8859-4") == 0) return 28594;
  if (strcasecmp(cs, "ISO-8859-5") == 0) return 28595;
  if (strcasecmp(cs, "ISO-8859-6") == 0) return 28596;
  if (strcasecmp(cs, "ISO-8859-7") == 0) return 28597;
  if (strcasecmp(cs, "ISO-8859-8") == 0) return 28598;
  if (strcasecmp(cs, "ISO-8859-9") == 0) return 28599;
  if (strcasecmp(cs, "ISO-8859-13") == 0) return 28603;
  if (strcasecmp(cs, "ISO-8859-15") == 0) return 28605;
  if (strcasecmp(cs, "KOI8-R") == 0) return 20866;
  if (strcasecmp(cs, "KOI8-U") == 0) return 21866;
  if (strcasecmp(cs, "MACINTOSH") == 0) return 10000;
  if (strcasecmp(cs, "IBM866") == 0 || strcasecmp(cs, "CP866") == 0) return 866;
  return 0;
}

moonbit_bytes_t fv_to_utf8(moonbit_bytes_t in, moonbit_bytes_t charset) {
  UINT cp = fv_win_codepage((const char*)charset);
  int32_t n = Moonbit_array_length(in);
  if (cp == 0 || n <= 0) return moonbit_make_bytes(0, 0);
  int wlen = MultiByteToWideChar(cp, 0, (const char*)in, n, NULL, 0);
  if (wlen <= 0) return moonbit_make_bytes(0, 0);
  wchar_t* wbuf = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
  if (!wbuf) return moonbit_make_bytes(0, 0);
  MultiByteToWideChar(cp, 0, (const char*)in, n, wbuf, wlen);
  int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
  if (ulen <= 0) {
    free(wbuf);
    return moonbit_make_bytes(0, 0);
  }
  moonbit_bytes_t out = moonbit_make_bytes(ulen, 0);
  WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, (char*)out, ulen, NULL, NULL);
  free(wbuf);
  return out;
}

moonbit_bytes_t fv_from_utf8(moonbit_bytes_t in, moonbit_bytes_t charset) {
  int url = strncmp((const char*)charset, "url:", 4) == 0;
  if (url) charset = (moonbit_bytes_t)((const char*)charset + 4);
  UINT cp = fv_win_codepage((const char*)charset);
  int32_t n = Moonbit_array_length(in);
  if (cp == 0) return moonbit_make_bytes(0, 0);
  if (n <= 0) return moonbit_make_bytes(0, 0);
  size_t cap = (size_t)n * 4 + 16, used = 0;
  char* buf = (char*)malloc(cap);
  const unsigned char* u = (const unsigned char*)in;
  int32_t i = 0;
  while (i < n) {
    int len = u[i] < 0x80 ? 1 : u[i] < 0xE0 ? 2 : u[i] < 0xF0 ? 3 : 4;
    if (i + len > n) len = n - i;
    if (cap - used < 32) { cap *= 2; buf = (char*)realloc(buf, cap); }
    wchar_t wch[2];
    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char*)(u + i), len, wch, 2);
    BOOL used_default = FALSE;
    char mb[8];
    int mn = wn > 0 ? WideCharToMultiByte(cp, 0, wch, wn, mb, (int)sizeof(mb), NULL, &used_default) : 0;
    if (mn > 0 && !used_default) {
      memcpy(buf + used, mb, (size_t)mn);
      used += (size_t)mn;
    } else {
      uint32_t cp_val = len == 1 ? u[i] : len == 2 ? ((u[i] & 0x1F) << 6) | (u[i + 1] & 0x3F)
        : len == 3 ? ((u[i] & 0x0F) << 12) | ((u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F)
        : ((u[i] & 0x07) << 18) | ((u[i + 1] & 0x3F) << 12) | ((u[i + 2] & 0x3F) << 6) | (u[i + 3] & 0x3F);
      used += (size_t)snprintf(buf + used, cap - used, url ? "%%26%%23%u%%3B" : "&#%u;", cp_val);
    }
    i += len;
  }
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)used, 0);
  if (used > 0) memcpy(out, buf, used);
  free(buf);
  return out;
}
#else
#include <iconv.h>
#include <errno.h>
moonbit_bytes_t fv_to_utf8(moonbit_bytes_t in, moonbit_bytes_t charset) {
  iconv_t cd = iconv_open("UTF-8", (const char*)charset);
  int32_t n = Moonbit_array_length(in);
  if (cd == (iconv_t)-1) return moonbit_make_bytes(0, 0);
  size_t cap = (size_t)n * 4 + 16;
  char* buf = malloc(cap);
  char* src = (char*)in; size_t left = (size_t)n;
  char* dst = buf; size_t room = cap;
  const char* cs = (const char*)charset;
  // single-byte encodings a byte at a time, as the Encoding Standard's indexes have them where iconv differs: the
  // windows-125x bytes it leaves undefined are the C1 controls, macintosh's 7F is DEL, and windows-874 has nothing
  // at DB-DE and FC-FF
  if (strncmp(cs, "WINDOWS-", 8) == 0 || strncmp(cs, "ISO-8859-", 9) == 0 || strncmp(cs, "KOI8", 4) == 0 || strcmp(cs, "MACINTOSH") == 0) {
    int w125 = strncmp(cs, "WINDOWS-125", 11) == 0, w874 = strcmp(cs, "WINDOWS-874") == 0, mac = strcmp(cs, "MACINTOSH") == 0;
    size_t used = 0;
    for (int32_t i = 0; i < n; i++) {
      unsigned char b = ((unsigned char*)in)[i];
      if (cap - used < 8) { cap *= 2; buf = realloc(buf, cap); }
      if (b < 0x80) { buf[used++] = (char)b; continue; }
      char* s1 = (char*)in + i; size_t l1 = 1; char* d1 = buf + used; size_t r1 = cap - used;
      int bad = w874 && ((b >= 0xDB && b <= 0xDE) || b >= 0xFC);
      if (!bad && iconv(cd, &s1, &l1, &d1, &r1) != (size_t)-1) { used = (size_t)(d1 - buf); continue; }
      iconv(cd, NULL, NULL, NULL, NULL);
      if (!bad && w125 && b <= 0x9F) { buf[used++] = (char)0xC2; buf[used++] = (char)b; continue; }
      buf[used++] = (char)0xEF; buf[used++] = (char)0xBF; buf[used++] = (char)0xBD;
    }
    (void)mac;
    dst = buf + used; left = 0;
  }
  while (left > 0) {
    size_t r = iconv(cd, &src, &left, &dst, &room);
    if (r != (size_t)-1) break;
    if (errno == E2BIG) {
      size_t used = (size_t)(dst - buf);
      cap *= 2; buf = realloc(buf, cap); dst = buf + used; room = cap - used;
    } else {
      // a byte that does not convert (or a cut-off sequence at the end): U+FFFD and on
      if (room < 3) { size_t used = (size_t)(dst - buf); cap *= 2; buf = realloc(buf, cap); dst = buf + used; room = cap - used; }
      *dst++ = (char)0xEF; *dst++ = (char)0xBF; *dst++ = (char)0xBD; room -= 3;
      src++; left--;
      iconv(cd, NULL, NULL, NULL, NULL);
    }
  }
  iconv_close(cd);
  size_t len = (size_t)(dst - buf);
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)len, 0);
  memcpy(out, buf, len);
  free(buf);
  return out;
}

// UTF-8 text in another encoding (iconv's name for it), as a URL's query or a form is sent in a page's encoding:
// a character the encoding has no bytes for is written as an HTML character reference (&#N;), as the Encoding
// Standard's encode does for URLs and forms. Empty when iconv knows no such encoding.
// A charset given as "url:NAME" writes such a character percent-encoded (%26%23N%3B), as a URL's query has it.
moonbit_bytes_t fv_from_utf8(moonbit_bytes_t in, moonbit_bytes_t charset) {
  int url = strncmp((const char*)charset, "url:", 4) == 0;
  if (url) charset = (moonbit_bytes_t)((const char*)charset + 4);
  iconv_t cd = iconv_open((const char*)charset, "UTF-8");
  int32_t n = Moonbit_array_length(in);
  if (cd == (iconv_t)-1) return moonbit_make_bytes(0, 0);
  size_t cap = (size_t)n * 4 + 16, used = 0;
  char* buf = malloc(cap);
  const unsigned char* u = (const unsigned char*)in;
  int32_t i = 0;
  int sjis = strstr((const char*)charset, "SHIFT") || strstr((const char*)charset, "SJIS") || strstr((const char*)charset, "932");
  // Big5 as the Encoding Standard encodes it: Big5 proper (CP950) first, then HKSCS's characters with lead bytes from
  // A1 on (its own area below A1 is never written)
  iconv_t big5 = strstr((const char*)charset, "BIG5") ? iconv_open("CP950", "UTF-8") : (iconv_t)-1;
  while (i < n) {
    int len = u[i] < 0x80 ? 1 : u[i] < 0xE0 ? 2 : u[i] < 0xF0 ? 3 : 4;
    if (i + len > n) len = n - i;
    if (cap - used < 32) { cap *= 2; buf = realloc(buf, cap); }
    char* src = (char*)u + i; size_t left = (size_t)len;
    char* dst = buf + used; size_t room = cap - used;
    // (Shift_JIS as the Encoding Standard has it: U+0080 is byte 80, the yen sign 5C and the overline 7E)
    if (sjis && len >= 2) {
      uint32_t c2 = len == 2 ? ((u[i] & 0x1F) << 6) | (u[i + 1] & 0x3F) : len == 3 ? ((u[i] & 0x0F) << 12) | ((u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F) : 0;
      int b = c2 == 0x80 ? 0x80 : c2 == 0xA5 ? 0x5C : c2 == 0x203E ? 0x7E : -1;
      if (b >= 0) { buf[used++] = (char)b; i += len; continue; }
      if (c2 == 0x2212) { buf[used++] = (char)0x81; buf[used++] = (char)0x7C; i += len; continue; }
    }
    // (not the private use area: the Encoding Standard's Big5 has no bytes for it)
    int pua = len == 3 && (u[i] == 0xEE || (u[i] == 0xEF && u[i + 1] < 0xA4));
    if (big5 != (iconv_t)-1 && len >= 2 && !pua) {
      char* s2 = src; size_t l2 = left; char* d2 = dst; size_t r2 = room;
      if (iconv(big5, &s2, &l2, &d2, &r2) != (size_t)-1 && d2 - dst == 2 && (unsigned char)dst[0] >= 0xA1) { used += 2; i += len; continue; }
      iconv(big5, NULL, NULL, NULL, NULL);
    }
    size_t r = iconv(cd, &src, &left, &dst, &room);
    if (r != (size_t)-1 && big5 != (iconv_t)-1 && len >= 2 && ((unsigned char)buf[used] < 0xA1 || pua)) r = (size_t)-1;
    if (r == (size_t)-1) {
      iconv(cd, NULL, NULL, NULL, NULL);
      uint32_t cp = len == 1 ? u[i] : len == 2 ? ((u[i] & 0x1F) << 6) | (u[i + 1] & 0x3F)
        : len == 3 ? ((u[i] & 0x0F) << 12) | ((u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F)
        : ((u[i] & 0x07) << 18) | ((u[i + 1] & 0x3F) << 12) | ((u[i + 2] & 0x3F) << 6) | (u[i + 3] & 0x3F);
      used += (size_t)snprintf(buf + used, cap - used, url ? "%%26%%23%u%%3B" : "&#%u;", cp);
    } else {
      used = (size_t)(dst - buf);
    }
    i += len;
  }
  iconv_close(cd);
  if (big5 != (iconv_t)-1) iconv_close(big5);
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)used, 0);
  memcpy(out, buf, used);
  free(buf);
  return out;
}
#endif /* _WIN32 */

// ============================================================================
// Streaming Large-File Resumable Downloader (O(1) memory, .part + Range/206)
// ============================================================================

#ifndef _WIN32
#include <sys/stat.h>
#include <strings.h>
#endif

#define FV_DL_NOT_FOUND   0
#define FV_DL_DOWNLOADING 1
#define FV_DL_COMPLETED   2
#define FV_DL_CANCELLED   3
#define FV_DL_FAILED      4

typedef struct {
  int32_t id;
  char* url;
  char* dest_path;
  char* part_path;
  char* headers;
  int64_t resume_from;
  int64_t bytes_received;
  int64_t session_bytes;
  int64_t total_bytes;
  int64_t content_range_total;
  int32_t http_status;
  int32_t state;
  int64_t start_us;
  int64_t end_us;
  char error[256];
  int cancel_requested;
  int remove_part_on_cancel;
  int worker_done;
  int header_decided;
  FILE* fp;
  CURL* easy;
  pthread_t thread;
  int thread_started;
} DownloadTask;

static pthread_mutex_t dl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dl_cv = PTHREAD_COND_INITIALIZER;
static DownloadTask** dl_tasks = NULL;
static int dl_count = 0;
static int dl_cap = 0;
static int32_t dl_next_id = 1;
static pthread_once_t dl_atexit_once = PTHREAD_ONCE_INIT;

static int dl_path_is_safe(const char* path) {
  if (!path || path[0] == '\0') return 0;
  size_t len = strlen(path);
  if (len == 0 || len > 4000) return 0;
  if (path[len - 1] == '/' || path[len - 1] == '\\') return 0;
  // Reject ".." path traversal segments
  const char* p = path;
  while (*p) {
    while (*p == '/' || *p == '\\') p++;
    if (!*p) break;
    const char* seg = p;
    while (*p && *p != '/' && *p != '\\') p++;
    size_t slen = (size_t)(p - seg);
    if (slen == 2 && seg[0] == '.' && seg[1] == '.') return 0;
  }
  return 1;
}

static int dl_url_is_valid(const char* url) {
  if (!url || url[0] == '\0') return 0;
  return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static void dl_set_error(DownloadTask* t, const char* msg) {
  if (t->error[0] == '\0' && msg) {
    snprintf(t->error, sizeof(t->error), "%s", msg);
  }
}

static size_t dl_header_cb(char* d, size_t s, size_t m, void* u) {
  DownloadTask* t = (DownloadTask*)u;
  size_t k = s * m;
  if (__atomic_load_n(&t->cancel_requested, __ATOMIC_ACQUIRE)) return 0;
  // Reset per-response metadata on each HTTP status line (e.g. redirects)
  if (k >= 5 && strncmp(d, "HTTP/", 5) == 0) {
    t->content_range_total = -1;
    return k;
  }
  if (k > 14 && strncasecmp(d, "Content-Range:", 14) == 0) {
    // Parse "Content-Range: bytes start-end/total"
    const char* slash = memchr(d + 14, '/', k - 14);
    if (slash) {
      const char* p = slash + 1;
      const char* end = d + k;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p < end && *p >= '0' && *p <= '9') {
        int64_t val = 0;
        while (p < end && *p >= '0' && *p <= '9') {
          val = val * 10 + (*p - '0');
          p++;
        }
        t->content_range_total = val;
      }
    }
  }
  return k;
}

static int dl_open_part_if_needed(DownloadTask* t) {
  if (t->header_decided) return t->fp ? 0 : -1;
  t->header_decided = 1;

  long status = 0;
  curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
  curl_off_t cl = -1;
  curl_easy_getinfo(t->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);

  pthread_mutex_lock(&dl_mu);
  t->http_status = (int32_t)status;
  pthread_mutex_unlock(&dl_mu);

  if (status < 200 || status >= 300) {
    char msg[64];
    snprintf(msg, sizeof(msg), "HTTP %ld", status);
    dl_set_error(t, msg);
    return -1;
  }

  if (t->resume_from > 0 && status == 206) {
    FILE* fp = fopen(t->part_path, "r+b");
    if (!fp) {
      dl_set_error(t, "resume failed: .part file missing");
      return -1;
    }
    if (fseeko(fp, 0, SEEK_END) != 0) {
      fclose(fp);
      dl_set_error(t, "resume failed: seek error");
      return -1;
    }
    off_t cur_sz = ftello(fp);
    if (cur_sz < (off_t)t->resume_from) {
      fclose(fp);
      dl_set_error(t, "resume failed: .part smaller than resume offset");
      return -1;
    }
    if (ftruncate(fileno(fp), (off_t)t->resume_from) != 0 ||
        fseeko(fp, (off_t)t->resume_from, SEEK_SET) != 0) {
      fclose(fp);
      dl_set_error(t, "resume failed: truncate/seek error");
      return -1;
    }
    setvbuf(fp, NULL, _IOFBF, 65536);
    t->fp = fp;
    pthread_mutex_lock(&dl_mu);
    t->bytes_received = t->resume_from;
    if (t->content_range_total > 0) {
      t->total_bytes = t->content_range_total;
    } else if (cl >= 0) {
      t->total_bytes = t->resume_from + (int64_t)cl;
    }
    pthread_mutex_unlock(&dl_mu);
  } else {
    // Fresh download or server responded 200 OK (ignoring Range): overwrite from byte 0
    FILE* fp = fopen(t->part_path, "wb");
    if (!fp) {
      dl_set_error(t, strerror(errno));
      return -1;
    }
    setvbuf(fp, NULL, _IOFBF, 65536);
    t->fp = fp;
    pthread_mutex_lock(&dl_mu);
    t->bytes_received = 0;
    t->total_bytes = (cl >= 0) ? (int64_t)cl : -1;
    pthread_mutex_unlock(&dl_mu);
  }
  return 0;
}

static size_t dl_write_cb(char* d, size_t s, size_t m, void* u) {
  DownloadTask* t = (DownloadTask*)u;
  size_t k = s * m;
  if (__atomic_load_n(&t->cancel_requested, __ATOMIC_ACQUIRE)) return 0;
  if (!t->header_decided) {
    if (dl_open_part_if_needed(t) != 0) return 0;
  }
  if (k == 0) return 0;
  size_t w = fwrite(d, 1, k, t->fp);
  if (w != k) {
    dl_set_error(t, strerror(errno));
    return 0;
  }
  pthread_mutex_lock(&dl_mu);
  t->bytes_received += (int64_t)w;
  t->session_bytes += (int64_t)w;
  pthread_mutex_unlock(&dl_mu);
  return w;
}

static int dl_xferinfo_cb(void* u, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  (void)ulnow;
  DownloadTask* t = (DownloadTask*)u;
  return __atomic_load_n(&t->cancel_requested, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static void* dl_worker_main(void* arg) {
  DownloadTask* t = (DownloadTask*)arg;
  CURL* c = curl_easy_init();
  if (!c) {
    pthread_mutex_lock(&dl_mu);
    dl_set_error(t, "curl_easy_init failed");
    t->end_us = fv_now_us();
    t->state = FV_DL_FAILED;
    t->worker_done = 1;
    pthread_cond_broadcast(&dl_cv);
    pthread_mutex_unlock(&dl_mu);
    return NULL;
  }
  t->easy = c;

  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_URL, t->url);
  fv_fetch_apply_system_proxy(c, t->url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);
  // Use curl-impersonate TLS fingerprint without adding Accept-Encoding that would
  // distort byte-range offsets on binary downloads.
  curl_easy_impersonate(c, "chrome152", 0);

  struct curl_slist* hs = NULL;
  if (t->resume_from > 0) {
    char range_hdr[64];
    snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%lld-", (long long)t->resume_from);
    hs = curl_slist_append(hs, range_hdr);
  }
  if (t->headers && t->headers[0] != '\0') {
    for (const char* p = t->headers; *p;) {
      const char* e = strchr(p, '\n');
      if (!e) e = p + strlen(p);
      size_t ln = (size_t)(e - p);
      while (ln > 0 && p[ln - 1] == '\r') ln--;
      if (ln > 0) {
        char* line = strndup(p, ln);
        if (line) {
          hs = curl_slist_append(hs, line);
          free(line);
        }
      }
      p = *e ? e + 1 : e;
    }
  }
  if (hs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);

  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, dl_header_cb);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, t);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_xferinfo_cb);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, t);

  CURLcode rc = curl_easy_perform(c);

  int cancelled = __atomic_load_n(&t->cancel_requested, __ATOMIC_ACQUIRE);
  if (!cancelled && rc == CURLE_OK && !t->header_decided) {
    (void)dl_open_part_if_needed(t);
  }

  int flush_err = 0;
  if (t->fp) {
    if (fflush(t->fp) != 0) flush_err = 1;
    if (fclose(t->fp) != 0) flush_err = 1;
    t->fp = NULL;
  }

  int32_t final_state = FV_DL_FAILED;
  if (cancelled) {
    if (__atomic_load_n(&t->remove_part_on_cancel, __ATOMIC_ACQUIRE)) {
      (void)unlink(t->part_path);
    }
    final_state = FV_DL_CANCELLED;
  } else if (rc != CURLE_OK) {
    dl_set_error(t, curl_easy_strerror(rc));
    final_state = FV_DL_FAILED;
  } else if (flush_err) {
    dl_set_error(t, "disk write/flush failed");
    final_state = FV_DL_FAILED;
  } else if (t->http_status < 200 || t->http_status >= 300) {
    char msg[64];
    snprintf(msg, sizeof(msg), "HTTP %d", t->http_status);
    dl_set_error(t, msg);
    final_state = FV_DL_FAILED;
  } else {
#ifdef _WIN32
    if (!MoveFileExA(t->part_path, t->dest_path, MOVEFILE_REPLACE_EXISTING)) {
      dl_set_error(t, "MoveFileExA failed");
      final_state = FV_DL_FAILED;
    } else {
      final_state = FV_DL_COMPLETED;
    }
#else
    if (rename(t->part_path, t->dest_path) != 0) {
      dl_set_error(t, strerror(errno));
      final_state = FV_DL_FAILED;
    } else {
      final_state = FV_DL_COMPLETED;
    }
#endif
  }

  curl_slist_free_all(hs);
  curl_easy_cleanup(c);
  t->easy = NULL;

  pthread_mutex_lock(&dl_mu);
  t->end_us = fv_now_us();
  if (final_state == FV_DL_COMPLETED && t->total_bytes < 0) {
    t->total_bytes = t->bytes_received;
  }
  t->state = final_state;
  t->worker_done = 1;
  pthread_cond_broadcast(&dl_cv);
  pthread_mutex_unlock(&dl_mu);
  return NULL;
}

static void fv_download_cleanup_all(void) {
  pthread_mutex_lock(&dl_mu);
  for (int i = 0; i < dl_count; i++) {
    __atomic_store_n(&dl_tasks[i]->cancel_requested, 1, __ATOMIC_RELEASE);
  }
  int n = dl_count;
  DownloadTask** snapshot = dl_tasks;
  dl_tasks = NULL;
  dl_count = 0;
  dl_cap = 0;
  pthread_mutex_unlock(&dl_mu);

  for (int i = 0; i < n; i++) {
    DownloadTask* t = snapshot[i];
    if (t->thread_started) {
      pthread_join(t->thread, NULL);
    }
    free(t->url);
    free(t->dest_path);
    free(t->part_path);
    free(t->headers);
    free(t);
  }
  free(snapshot);
}

static void dl_register_atexit(void) {
  atexit(fv_download_cleanup_all);
}

int32_t fv_download_start(moonbit_bytes_t url, moonbit_bytes_t dest_path, moonbit_bytes_t headers, int64_t resume_from_bytes) {
  const char* u = (const char*)url;
  const char* dest = (const char*)dest_path;
  const char* hdrs = headers ? (const char*)headers : "";
  if (!dl_url_is_valid(u) || !dl_path_is_safe(dest)) return -1;
  if (resume_from_bytes < 0) return -1;

  pthread_once(&dl_atexit_once, dl_register_atexit);

  size_t dlen = strlen(dest);
  char* part = (char*)malloc(dlen + 6);
  if (!part) return -1;
  memcpy(part, dest, dlen);
  memcpy(part + dlen, ".part", 6);

  DownloadTask* t = (DownloadTask*)calloc(1, sizeof(DownloadTask));
  if (!t) {
    free(part);
    return -1;
  }
  t->url = strdup(u);
  t->dest_path = strdup(dest);
  t->part_path = part;
  t->headers = strdup(hdrs);
  t->resume_from = resume_from_bytes;
  t->bytes_received = resume_from_bytes;
  t->total_bytes = -1;
  t->content_range_total = -1;
  t->state = FV_DL_DOWNLOADING;
  t->start_us = fv_now_us();

  if (!t->url || !t->dest_path || !t->headers) {
    free(t->url);
    free(t->dest_path);
    free(t->part_path);
    free(t->headers);
    free(t);
    return -1;
  }

  pthread_mutex_lock(&dl_mu);
  if (dl_count == dl_cap) {
    int ncap = dl_cap ? dl_cap * 2 : 16;
    DownloadTask** nt = (DownloadTask**)realloc(dl_tasks, (size_t)ncap * sizeof(DownloadTask*));
    if (!nt) {
      pthread_mutex_unlock(&dl_mu);
      free(t->url);
      free(t->dest_path);
      free(t->part_path);
      free(t->headers);
      free(t);
      return -1;
    }
    dl_tasks = nt;
    dl_cap = ncap;
  }
  t->id = dl_next_id++;
  dl_tasks[dl_count++] = t;

  if (pthread_create(&t->thread, NULL, dl_worker_main, t) != 0) {
    dl_count--;
    pthread_mutex_unlock(&dl_mu);
    free(t->url);
    free(t->dest_path);
    free(t->part_path);
    free(t->headers);
    free(t);
    return -1;
  }
  t->thread_started = 1;
  int32_t id = t->id;
  pthread_mutex_unlock(&dl_mu);
  return id;
}

static unsigned char* dl_put64_be(unsigned char* o, int64_t v) {
  uint64_t uv = (uint64_t)v;
  o[0] = (unsigned char)(uv >> 56);
  o[1] = (unsigned char)(uv >> 48);
  o[2] = (unsigned char)(uv >> 40);
  o[3] = (unsigned char)(uv >> 32);
  o[4] = (unsigned char)(uv >> 24);
  o[5] = (unsigned char)(uv >> 16);
  o[6] = (unsigned char)(uv >> 8);
  o[7] = (unsigned char)uv;
  return o + 8;
}

static unsigned char* dl_put32_be(unsigned char* o, uint32_t v) {
  o[0] = (unsigned char)(v >> 24);
  o[1] = (unsigned char)(v >> 16);
  o[2] = (unsigned char)(v >> 8);
  o[3] = (unsigned char)v;
  return o + 4;
}

moonbit_bytes_t fv_download_poll(int32_t id) {
  int32_t state = FV_DL_NOT_FOUND;
  int64_t bytes_received = 0;
  int64_t total_bytes = -1;
  int32_t http_status = 0;
  int64_t speed_bps = 0;
  char err[256];
  err[0] = '\0';

  pthread_mutex_lock(&dl_mu);
  for (int i = 0; i < dl_count; i++) {
    if (dl_tasks[i]->id == id) {
      DownloadTask* t = dl_tasks[i];
      state = t->state;
      bytes_received = t->bytes_received;
      total_bytes = t->total_bytes;
      http_status = t->http_status;
      int64_t ref_us = t->worker_done ? t->end_us : fv_now_us();
      int64_t elapsed_us = ref_us - t->start_us;
      if (elapsed_us > 0 && t->session_bytes > 0) {
        speed_bps = (int64_t)((double)t->session_bytes * 1000000.0 / (double)elapsed_us);
      }
      snprintf(err, sizeof(err), "%s", t->error);
      break;
    }
  }
  pthread_mutex_unlock(&dl_mu);

  size_t elen = strlen(err);
  size_t total = 1 + 8 + 8 + 4 + 8 + 4 + elen;
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)total, 0);
  unsigned char* o = (unsigned char*)out;
  *o++ = (unsigned char)state;
  o = dl_put64_be(o, bytes_received);
  o = dl_put64_be(o, total_bytes);
  o = dl_put32_be(o, (uint32_t)http_status);
  o = dl_put64_be(o, speed_bps);
  o = dl_put32_be(o, (uint32_t)elen);
  if (elen > 0) memcpy(o, err, elen);
  return out;
}

int32_t fv_download_cancel(int32_t id, int32_t remove_part) {
  pthread_mutex_lock(&dl_mu);
  DownloadTask* t = NULL;
  for (int i = 0; i < dl_count; i++) {
    if (dl_tasks[i]->id == id) {
      t = dl_tasks[i];
      break;
    }
  }
  if (!t) {
    pthread_mutex_unlock(&dl_mu);
    return 0;
  }
  __atomic_store_n(&t->remove_part_on_cancel, remove_part ? 1 : 0, __ATOMIC_RELEASE);
  __atomic_store_n(&t->cancel_requested, 1, __ATOMIC_RELEASE);
  while (!t->worker_done) {
    pthread_cond_wait(&dl_cv, &dl_mu);
  }
  if (remove_part && t->state == FV_DL_CANCELLED) {
    (void)unlink(t->part_path);
  }
  pthread_mutex_unlock(&dl_mu);
  return 1;
}
