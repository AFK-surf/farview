#include "c_compat/moonbit.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include "../../../native/win_compat.h"
#else
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#endif

int32_t pion_fetch_mb_len(void *b) {
  return fv_mb_bytes_len((moonbit_bytes_t)b);
}

void pion_fetch_mb_free(void *b) {
  fv_mb_bytes_free((moonbit_bytes_t)b);
}

const void *pion_fetch_curl_version(void) {
  return (const void *)curl_version();
}

const void *pion_fetch_getenv(const void *name_cstr) {
  const char *v = getenv((const char *)name_cstr);
  return (const void *)(v ? v : "");
}

int32_t pion_fetch_has_env(const void *name_cstr) {
  return getenv((const char *)name_cstr) != NULL ? 1 : 0;
}

// Minimal single-use loopback HTTP server for self-contained unit tests.
typedef struct {
  int srv_fd;
  int max_reqs;
} fv_test_http_cfg;

static void *fv_test_http_thread(void *arg) {
  fv_test_http_cfg *cfg = (fv_test_http_cfg *)arg;
  int srv = cfg->srv_fd;
  int max_reqs = cfg->max_reqs;
  free(cfg);

  for (int i = 0; i < max_reqs; i++) {
    fv_sock_t cs = accept(fv_sock_from_i32(srv), NULL, NULL);
    if (cs == FV_SOCK_INVALID) break;
    int c = fv_sock_to_i32(cs);
    char req[8192];
    ssize_t n = fv_sock_recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) {
      fv_sock_close(c);
      continue;
    }
    req[n] = 0;

    char resp[4096];
    int rlen = 0;
    if (strncmp(req, "GET /hello ", 11) == 0) {
      const char *body = "hello-from-loopback";
      rlen = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain; charset=utf-8\r\n"
                      "Content-Disposition: inline; filename=\"hello.txt\"\r\n"
                      "Set-Cookie: sid=pion42; Path=/\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(body), body);
    } else if (strncmp(req, "POST /echo ", 11) == 0) {
      const char *body = strstr(req, "\r\n\r\n");
      body = body ? body + 4 : "";
      int has_cookie = strstr(req, "sid=pion42") != NULL ? 1 : 0;
      char out_body[1024];
      int oblen = snprintf(out_body, sizeof(out_body), "cookie=%d|body=%s", has_cookie, body);
      rlen = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 201 Created\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "X-Test-Reply: ok\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n\r\n%s",
                      oblen, out_body);
    } else {
      const char *body = "not-found";
      rlen = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 404 Not Found\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      strlen(body), body);
    }
    if (rlen > 0) {
      (void)fv_sock_send(c, resp, (size_t)rlen, 0);
    }
    fv_sock_close(c);
  }
  fv_sock_close(srv);
  return NULL;
}

int32_t pion_fetch_test_start_http(int32_t max_reqs) {
  fv_win_wsa_init();
  fv_sock_t srvs = socket(AF_INET, SOCK_STREAM, 0);
  if (srvs == FV_SOCK_INVALID) return -1;
  int srv = fv_sock_to_i32(srvs);
  int on = 1;
  setsockopt(srvs, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(srvs, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srvs, 8) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  socklen_t alen = sizeof(a);
  if (getsockname(srvs, (struct sockaddr *)&a, &alen) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  int port = ntohs(a.sin_port);
  fv_test_http_cfg *cfg = (fv_test_http_cfg *)malloc(sizeof(fv_test_http_cfg));
  cfg->srv_fd = srv;
  cfg->max_reqs = max_reqs;
  pthread_t tid;
  if (pthread_create(&tid, NULL, fv_test_http_thread, cfg) != 0) {
    free(cfg);
    fv_sock_close(srv);
    return -1;
  }
  pthread_detach(tid);
  return port;
}

void pion_fetch_sleep_ms(int32_t ms) {
  if (ms > 0) usleep((uint64_t)ms * 1000ULL);
}

int64_t pion_fetch_test_file_size(const void *path_cstr) {
  struct stat st;
  if (stat((const char *)path_cstr, &st) != 0) return -1;
  return (int64_t)st.st_size;
}

int32_t pion_fetch_test_unlink(const void *path_cstr) {
  return unlink((const char *)path_cstr) == 0 ? 1 : 0;
}

// Verify deterministic pattern byte[i] = (uint8_t)((i * 31 + 17) & 0xFF) over entire file in O(1) memory.
int32_t pion_fetch_test_verify_pattern(const void *path_cstr, int64_t expected_len) {
  FILE *fp = fopen((const char *)path_cstr, "rb");
  if (!fp) return 0;
  unsigned char buf[65536];
  int64_t off = 0;
  while (1) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      unsigned char expect = (unsigned char)(((uint64_t)(off + (int64_t)i) * 31ULL + 17ULL) & 0xFFULL);
      if (buf[i] != expect) {
        fclose(fp);
        return 0;
      }
    }
    off += (int64_t)n;
  }
  fclose(fp);
  return off == expected_len ? 1 : 0;
}

// Streaming Range-capable loopback HTTP server for self-contained download tests (O(1) server memory).
typedef struct {
  int srv_fd;
  int max_reqs;
  int64_t total_len;
  int slow_ms_per_chunk;
} fv_test_dl_cfg;

static void *fv_test_dl_thread(void *arg) {
  fv_test_dl_cfg *cfg = (fv_test_dl_cfg *)arg;
  int srv = cfg->srv_fd;
  int max_reqs = cfg->max_reqs;
  int64_t total_len = cfg->total_len;
  int slow_ms = cfg->slow_ms_per_chunk;
  free(cfg);

  unsigned char chunk[65536];
  for (int i = 0; i < max_reqs; i++) {
    fv_sock_t cs = accept(fv_sock_from_i32(srv), NULL, NULL);
    if (cs == FV_SOCK_INVALID) break;
    int c = fv_sock_to_i32(cs);
    char req[8192];
    ssize_t n = fv_sock_recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) {
      fv_sock_close(c);
      continue;
    }
    req[n] = 0;

    int ignore_range = strstr(req, "GET /norange ") != NULL;
    int is_slow = strstr(req, "GET /slow") != NULL;
    int64_t start = 0;
    int has_range = 0;
    if (!ignore_range) {
      const char *rh = strstr(req, "Range: bytes=");
      if (!rh) rh = strstr(req, "range: bytes=");
      if (rh) {
        start = strtoll(rh + 13, NULL, 10);
        if (start < 0) start = 0;
        if (start > total_len) start = total_len;
        has_range = 1;
      }
    }

#ifdef SO_NOSIGPIPE
    {
      int one = 1;
      setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
    }
#endif

    char hdr[512];
    int hlen = 0;
    int64_t send_len = total_len - start;
    if (has_range && start > 0 && start < total_len) {
      hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Content-Range: bytes %lld-%lld/%lld\r\n"
                      "Content-Length: %lld\r\n"
                      "Connection: close\r\n\r\n",
                      (long long)start, (long long)(total_len - 1),
                      (long long)total_len, (long long)send_len);
    } else {
      start = 0;
      send_len = total_len;
      hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Content-Length: %lld\r\n"
                      "Connection: close\r\n\r\n",
                      (long long)send_len);
    }
    if (hlen > 0) {
      (void)fv_sock_send(c, hdr, (size_t)hlen, 0);
    }

    int64_t off = start;
    while (off < total_len) {
      size_t want = sizeof(chunk);
      if ((int64_t)want > total_len - off) want = (size_t)(total_len - off);
      for (size_t k = 0; k < want; k++) {
        chunk[k] = (unsigned char)(((uint64_t)(off + (int64_t)k) * 31ULL + 17ULL) & 0xFFULL);
      }
      size_t sent = 0;
      int broken = 0;
      while (sent < want) {
#ifdef MSG_NOSIGNAL
        ssize_t w = fv_sock_send(c, chunk + sent, want - sent, MSG_NOSIGNAL);
#else
        ssize_t w = fv_sock_send(c, chunk + sent, want - sent, 0);
#endif
        if (w <= 0) {
          broken = 1;
          break;
        }
        sent += (size_t)w;
      }
      if (broken) break;
      off += (int64_t)want;
      if (is_slow && slow_ms > 0) usleep((uint64_t)slow_ms * 1000ULL);
    }
    fv_sock_close(c);
  }
  fv_sock_close(srv);
  return NULL;
}

int32_t pion_fetch_test_start_dl_http(int32_t max_reqs, int64_t total_len, int32_t slow_ms) {
  fv_win_wsa_init();
  fv_sock_t srvs = socket(AF_INET, SOCK_STREAM, 0);
  if (srvs == FV_SOCK_INVALID) return -1;
  int srv = fv_sock_to_i32(srvs);
  int on = 1;
  setsockopt(srvs, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(srvs, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srvs, 8) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  socklen_t alen = sizeof(a);
  if (getsockname(srvs, (struct sockaddr *)&a, &alen) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  int port = ntohs(a.sin_port);
  fv_test_dl_cfg *cfg = (fv_test_dl_cfg *)malloc(sizeof(fv_test_dl_cfg));
  cfg->srv_fd = srv;
  cfg->max_reqs = max_reqs;
  cfg->total_len = total_len;
  cfg->slow_ms_per_chunk = slow_ms;
  pthread_t tid;
  if (pthread_create(&tid, NULL, fv_test_dl_thread, cfg) != 0) {
    free(cfg);
    fv_sock_close(srv);
    return -1;
  }
  pthread_detach(tid);
  return port;
}
