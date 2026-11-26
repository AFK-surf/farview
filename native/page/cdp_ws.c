#include "cdp_ws.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "../win_compat.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define FV_CDP_MAX_SERVERS 8
#define FV_CDP_MAX_CLIENTS 16
#define FV_CDP_MAX_FRAME (4 * 1024 * 1024) /* 4 MiB max single frame to prevent OOM */
#define FV_CDP_MAX_HTTP_HDR 16384

/* -------------------- Minimal pure-C SHA-1 (FIPS 180-4) -------------------- */

static uint32_t sha1_rol(uint32_t v, int bits) {
  return (v << bits) | (v >> (32 - bits));
}

static void cdp_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
  uint32_t h0 = 0x67452301u;
  uint32_t h1 = 0xEFCDAB89u;
  uint32_t h2 = 0x98BADCFEu;
  uint32_t h3 = 0x10325476u;
  uint32_t h4 = 0xC3D2E1F0u;

  uint64_t bit_len = (uint64_t)len * 8u;
  size_t total = ((len + 9u + 63u) / 64u) * 64u;
  uint8_t *msg = (uint8_t *)calloc(1, total);
  if (!msg) {
    memset(out, 0, 20);
    return;
  }
  if (len > 0) memcpy(msg, data, len);
  msg[len] = 0x80u;
  for (int i = 0; i < 8; i++) {
    msg[total - 1 - i] = (uint8_t)((bit_len >> (i * 8)) & 0xFFu);
  }

  for (size_t off = 0; off < total; off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      const uint8_t *p = msg + off + i * 4;
      w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
             ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (int i = 16; i < 80; i++) {
      w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      uint32_t tmp = sha1_rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = sha1_rol(b, 30);
      b = a;
      a = tmp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  free(msg);

  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++) {
    out[i * 4 + 0] = (uint8_t)((hs[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = (uint8_t)((hs[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = (uint8_t)((hs[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = (uint8_t)(hs[i] & 0xFFu);
  }
}

static size_t cdp_base64_encode(const uint8_t *in, size_t len, char *out) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0, j = 0;
  while (i < len) {
    uint32_t b0 = in[i++];
    uint32_t b1 = (i < len) ? in[i++] : 0;
    uint32_t b2 = (i < len) ? in[i++] : 0;
    size_t rem = len - (i - ((i <= len) ? 0 : 0));
    (void)rem;
    out[j++] = tbl[(b0 >> 2) & 63];
    out[j++] = tbl[((b0 & 3) << 4) | ((b1 >> 4) & 15)];
    out[j++] = tbl[((b1 & 15) << 2) | ((b2 >> 6) & 3)];
    out[j++] = tbl[b2 & 63];
  }
  size_t mod = len % 3;
  if (mod == 1) {
    out[j - 2] = '=';
    out[j - 1] = '=';
  } else if (mod == 2) {
    out[j - 1] = '=';
  }
  out[j] = '\0';
  return j;
}

int32_t fv_cdp_ws_accept_key(const void *key, int32_t key_len, void *out_buf, int32_t cap) {
  if (!key || key_len <= 0 || key_len > 256 || !out_buf || cap < 29) return -1;
  static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char concat[320];
  memcpy(concat, key, (size_t)key_len);
  memcpy(concat + key_len, guid, sizeof(guid) - 1);
  uint8_t digest[20];
  cdp_sha1((const uint8_t *)concat, (size_t)key_len + sizeof(guid) - 1, digest);
  char b64[32];
  size_t n = cdp_base64_encode(digest, 20, b64);
  if ((int32_t)n >= cap) return -1;
  memcpy(out_buf, b64, n);
  ((char *)out_buf)[n] = '\0';
  return (int32_t)n;
}

/* -------------------- Socket & Buffer Helpers -------------------- */

static int set_nonblock(int fd) {
#ifdef _WIN32
  return fv_sock_set_nb(fd, 1);
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int send_all_nonblock(int fd, const uint8_t *buf, size_t len) {
  size_t sent = 0;
  int spins = 0;
  while (sent < len) {
#ifdef _WIN32
    int n = fv_sock_send(fd, buf + sent, (int)(len - sent), 0);
#else
    ssize_t n = send(fd, buf + sent, len - sent,
#ifdef MSG_NOSIGNAL
                     MSG_NOSIGNAL
#else
                     0
#endif
    );
#endif
    if (n > 0) {
      sent += (size_t)n;
      spins = 0;
      continue;
    }
    if (n < 0 && fv_sock_eintr()) continue;
    if (n < 0 && fv_sock_wouldblock()) {
      if (++spins > 200) return -1;
#ifdef _WIN32
      WSAPOLLFD pfd;
      pfd.fd = fv_sock_from_i32(fd);
      pfd.events = POLLOUT;
      pfd.revents = 0;
      WSAPoll(&pfd, 1, 5);
#else
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      poll(&pfd, 1, 5);
#endif
      continue;
    }
    return -1;
  }
  return (int)sent;
}

static int send_ws_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
  if (len > FV_CDP_MAX_FRAME) return -1;
  uint8_t hdr[10];
  size_t hlen = 0;
  hdr[0] = (uint8_t)(0x80u | (opcode & 0x0Fu));
  if (len < 126) {
    hdr[1] = (uint8_t)len;
    hlen = 2;
  } else if (len <= 0xFFFFu) {
    hdr[1] = 126;
    hdr[2] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[3] = (uint8_t)(len & 0xFFu);
    hlen = 4;
  } else {
    hdr[1] = 127;
    uint64_t l64 = (uint64_t)len;
    for (int i = 0; i < 8; i++) {
      hdr[2 + i] = (uint8_t)((l64 >> ((7 - i) * 8)) & 0xFFu);
    }
    hlen = 10;
  }
  if (len <= 65536) {
    uint8_t *buf = (uint8_t *)malloc(hlen + len);
    if (buf) {
      memcpy(buf, hdr, hlen);
      if (len > 0) memcpy(buf + hlen, payload, len);
      int rc = send_all_nonblock(fd, buf, hlen + len);
      free(buf);
      return rc < 0 ? -1 : (int)len;
    }
  }
  if (send_all_nonblock(fd, hdr, hlen) < 0) return -1;
  if (len > 0 && send_all_nonblock(fd, payload, len) < 0) return -1;
  return (int)len;
}

static void json_escape_into(const char *src, char *dst, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; src && src[i] && j + 6 < cap; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') {
      dst[j++] = '\\';
      dst[j++] = (char)c;
    } else if (c == '\n') {
      dst[j++] = '\\';
      dst[j++] = 'n';
    } else if (c == '\r') {
      dst[j++] = '\\';
      dst[j++] = 'r';
    } else if (c == '\t') {
      dst[j++] = '\\';
      dst[j++] = 't';
    } else if (c >= 0x20) {
      dst[j++] = (char)c;
    }
  }
  if (cap > 0) dst[j < cap ? j : cap - 1] = '\0';
}

/* -------------------- Server & Client State -------------------- */

typedef struct {
  int fd;
  int id;
  int upgraded; /* 0 = HTTP, 1 = WebSocket */
  uint8_t *rx;
  size_t rx_len;
  size_t rx_cap;
} CdpClient;

typedef struct {
  int in_use;
  int listen_fd;
  int port;
  int next_client_id;
  char title[256];
  char url[1024];
  CdpClient clients[FV_CDP_MAX_CLIENTS];
} CdpServer;

static pthread_mutex_t g_cdp_mu = PTHREAD_MUTEX_INITIALIZER;
static CdpServer g_servers[FV_CDP_MAX_SERVERS];

static void client_reset(CdpClient *c) {
  if (c->fd >= 0) {
    fv_sock_close(c->fd);
    c->fd = -1;
  }
  if (c->rx) {
    free(c->rx);
    c->rx = NULL;
  }
  c->rx_len = 0;
  c->rx_cap = 0;
  c->id = 0;
  c->upgraded = 0;
}

static int client_append_rx(CdpClient *c, const uint8_t *data, size_t n) {
  if (c->rx_len + n > FV_CDP_MAX_FRAME + 64) return -1;
  if (c->rx_len + n > c->rx_cap) {
    size_t ncap = c->rx_cap ? c->rx_cap * 2 : 4096;
    while (ncap < c->rx_len + n) ncap *= 2;
    if (ncap > FV_CDP_MAX_FRAME + 128) ncap = FV_CDP_MAX_FRAME + 128;
    uint8_t *p = (uint8_t *)realloc(c->rx, ncap);
    if (!p) return -1;
    c->rx = p;
    c->rx_cap = ncap;
  }
  memcpy(c->rx + c->rx_len, data, n);
  c->rx_len += n;
  return 0;
}

static void client_consume_rx(CdpClient *c, size_t n) {
  if (n >= c->rx_len) {
    c->rx_len = 0;
    return;
  }
  memmove(c->rx, c->rx + n, c->rx_len - n);
  c->rx_len -= n;
}

static int ascii_strncasecmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
    if (ca != cb) return (int)ca - (int)cb;
    if (ca == 0) return 0;
  }
  return 0;
}

static int find_header_value(const char *hdrs, size_t hdr_len, const char *name,
                             char *out, size_t out_cap) {
  size_t nlen = strlen(name);
  size_t i = 0;
  while (i < hdr_len) {
    size_t line_start = i;
    while (i < hdr_len && hdrs[i] != '\n') i++;
    size_t line_end = i;
    if (i < hdr_len && hdrs[i] == '\n') i++;
    if (line_end > line_start && hdrs[line_end - 1] == '\r') line_end--;
    size_t llen = line_end - line_start;
    if (llen > nlen + 1 &&
        ascii_strncasecmp(hdrs + line_start, name, nlen) == 0 &&
        hdrs[line_start + nlen] == ':') {
      size_t v = line_start + nlen + 1;
      while (v < line_end && (hdrs[v] == ' ' || hdrs[v] == '\t')) v++;
      size_t vend = line_end;
      while (vend > v && (hdrs[vend - 1] == ' ' || hdrs[vend - 1] == '\t')) vend--;
      size_t vlen = vend - v;
      if (vlen >= out_cap) vlen = out_cap - 1;
      memcpy(out, hdrs + v, vlen);
      out[vlen] = '\0';
      return (int)vlen;
    }
  }
  return -1;
}

static void send_http_json(int fd, int status, const char *status_text, const char *body) {
  size_t blen = strlen(body);
  char hdr[512];
  int hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      status, status_text, blen);
  if (hlen > 0) {
    size_t total = (size_t)hlen + blen;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (buf) {
      memcpy(buf, hdr, (size_t)hlen);
      memcpy(buf + hlen, body, blen);
      send_all_nonblock(fd, buf, total);
      free(buf);
    } else {
      send_all_nonblock(fd, (const uint8_t *)hdr, (size_t)hlen);
      send_all_nonblock(fd, (const uint8_t *)body, blen);
    }
  }
}

/* Returns 1 if client should be closed after HTTP response, 0 if upgraded/waiting. */
static int handle_http_request(CdpServer *srv, CdpClient *c) {
  if (c->rx_len < 4) return 0;
  /* Find \r\n\r\n */
  size_t hdr_end = 0;
  for (size_t i = 0; i + 3 < c->rx_len; i++) {
    if (c->rx[i] == '\r' && c->rx[i + 1] == '\n' &&
        c->rx[i + 2] == '\r' && c->rx[i + 3] == '\n') {
      hdr_end = i + 4;
      break;
    }
  }
  if (hdr_end == 0) {
    if (c->rx_len > FV_CDP_MAX_HTTP_HDR) return 1;
    return 0;
  }

  const char *req = (const char *)c->rx;
  /* Parse request line: GET <path> HTTP/1.1 */
  char path[256] = {0};
  const char *sp1 = (const char *)memchr(req, ' ', hdr_end);
  if (!sp1) return 1;
  const char *pstart = sp1 + 1;
  const char *sp2 = (const char *)memchr(pstart, ' ', hdr_end - (size_t)(pstart - req));
  if (!sp2) return 1;
  size_t plen = (size_t)(sp2 - pstart);
  if (plen >= sizeof(path)) plen = sizeof(path) - 1;
  memcpy(path, pstart, plen);
  path[plen] = '\0';

  /* Check WebSocket upgrade */
  char ws_key[128] = {0};
  char upgrade[64] = {0};
  if (find_header_value(req, hdr_end, "Upgrade", upgrade, sizeof(upgrade)) > 0 &&
      ascii_strncasecmp(upgrade, "websocket", 9) == 0 &&
      find_header_value(req, hdr_end, "Sec-WebSocket-Key", ws_key, sizeof(ws_key)) > 0) {
    char accept[64] = {0};
    if (fv_cdp_ws_accept_key(ws_key, (int32_t)strlen(ws_key), accept, sizeof(accept)) > 0) {
      char resp[512];
      int rlen = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
      if (rlen > 0 && send_all_nonblock(c->fd, (const uint8_t *)resp, (size_t)rlen) > 0) {
        c->upgraded = 1;
        client_consume_rx(c, hdr_end);
        return 0;
      }
    }
    return 1;
  }

  /* HTTP discovery endpoints */
  if (strcmp(path, "/json/version") == 0) {
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"Browser\":\"Farview/1.0 (Chrome/131.0.0.0)\","
             "\"Protocol-Version\":\"1.3\","
             "\"User-Agent\":\"Mozilla/5.0 Farview/1.0\","
             "\"V8-Version\":\"13.1\","
             "\"webSocketDebuggerUrl\":\"ws://127.0.0.1:%d/devtools/browser/farview\"}",
             srv->port);
    send_http_json(c->fd, 200, "OK", body);
    return 1;
  }
  if (strcmp(path, "/json/list") == 0 || strcmp(path, "/json") == 0) {
    char esc_title[512];
    char esc_url[2048];
    json_escape_into(srv->title[0] ? srv->title : "Farview", esc_title, sizeof(esc_title));
    json_escape_into(srv->url[0] ? srv->url : "about:blank", esc_url, sizeof(esc_url));
    char body[3072];
    snprintf(body, sizeof(body),
             "[{\"description\":\"\",\"devtoolsFrontendUrl\":\"\","
             "\"id\":\"page-1\",\"title\":\"%s\",\"type\":\"page\",\"url\":\"%s\","
             "\"webSocketDebuggerUrl\":\"ws://127.0.0.1:%d/devtools/page/1\"}]",
             esc_title, esc_url, srv->port);
    send_http_json(c->fd, 200, "OK", body);
    return 1;
  }

  send_http_json(c->fd, 404, "Not Found", "{\"error\":\"Not Found\"}");
  return 1;
}

/*
 * Try to parse one RFC 6455 frame from c->rx.
 * Returns:
 *   > 0 : text/binary payload copied to out_buf (returns payload len)
 *     0 : need more data (or handled control frame PING/PONG internally)
 *    -1 : protocol error or CLOSE frame -> caller should close client
 */
static int try_read_ws_frame(CdpClient *c, void *out_buf, int32_t cap) {
  while (c->rx_len >= 2) {
    uint8_t b0 = c->rx[0];
    uint8_t b1 = c->rx[1];
    uint8_t opcode = b0 & 0x0Fu;
    int masked = (b1 & 0x80u) != 0;
    uint64_t plen = (uint64_t)(b1 & 0x7Fu);
    size_t pos = 2;

    if (plen == 126) {
      if (c->rx_len < 4) return 0;
      plen = ((uint64_t)c->rx[2] << 8) | (uint64_t)c->rx[3];
      pos = 4;
    } else if (plen == 127) {
      if (c->rx_len < 10) return 0;
      plen = 0;
      for (int i = 0; i < 8; i++) {
        plen = (plen << 8) | (uint64_t)c->rx[2 + i];
      }
      pos = 10;
    }

    if (plen > (uint64_t)FV_CDP_MAX_FRAME) {
      return -1; /* Reject oversized frame */
    }

    size_t mask_len = masked ? 4u : 0u;
    size_t total = pos + mask_len + (size_t)plen;
    if (c->rx_len < total) return 0;

    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
      memcpy(mask_key, c->rx + pos, 4);
      pos += 4;
    }

    uint8_t *payload = c->rx + pos;
    if (masked) {
      for (size_t i = 0; i < (size_t)plen; i++) {
        payload[i] ^= mask_key[i & 3u];
      }
    }

    if (opcode == 0x8u) {
      /* CLOSE frame */
      send_ws_frame(c->fd, 0x8u, NULL, 0);
      client_consume_rx(c, total);
      return -1;
    }
    if (opcode == 0x9u) {
      /* PING -> PONG */
      send_ws_frame(c->fd, 0xAu, payload, (size_t)plen);
      client_consume_rx(c, total);
      continue;
    }
    if (opcode == 0xAu) {
      /* PONG -> ignore */
      client_consume_rx(c, total);
      continue;
    }
    if (opcode == 0x1u || opcode == 0x2u) {
      int32_t copy_len = (int32_t)plen;
      if (copy_len > cap) copy_len = cap;
      if (copy_len > 0 && out_buf) {
        memcpy(out_buf, payload, (size_t)copy_len);
      }
      client_consume_rx(c, total);
      return copy_len > 0 ? copy_len : 0;
    }

    /* Unknown/unsupported opcode: drop frame */
    client_consume_rx(c, total);
  }
  return 0;
}

/* -------------------- Public C API -------------------- */

int32_t fv_cdp_listen(int32_t port) {
  if (port < 0 || port > 65535) return -1;
  fv_win_wsa_init();
  fv_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == FV_SOCK_INVALID) return -1;
  int fd = fv_sock_to_i32(s);

  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
  if (set_nonblock(fd) < 0) {
    fv_sock_close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* STRICT 127.0.0.1 loopback only */
  addr.sin_port = htons((uint16_t)port);

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fv_sock_close(fd);
    return -1;
  }
  if (listen(s, 16) < 0) {
    fv_sock_close(fd);
    return -1;
  }

  socklen_t alen = sizeof(addr);
  if (getsockname(s, (struct sockaddr *)&addr, &alen) < 0) {
    fv_sock_close(fd);
    return -1;
  }
  int bound_port = (int)ntohs(addr.sin_port);

  pthread_mutex_lock(&g_cdp_mu);
  int slot = -1;
  for (int i = 0; i < FV_CDP_MAX_SERVERS; i++) {
    if (!g_servers[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&g_cdp_mu);
    fv_sock_close(fd);
    return -1;
  }
  CdpServer *srv = &g_servers[slot];
  memset(srv, 0, sizeof(*srv));
  srv->in_use = 1;
  srv->listen_fd = fd;
  srv->port = bound_port;
  srv->next_client_id = 1;
  snprintf(srv->title, sizeof(srv->title), "Farview");
  snprintf(srv->url, sizeof(srv->url), "about:blank");
  for (int i = 0; i < FV_CDP_MAX_CLIENTS; i++) {
    srv->clients[i].fd = -1;
  }
  pthread_mutex_unlock(&g_cdp_mu);
  return slot + 1;
}

int32_t fv_cdp_port(int32_t handle) {
  if (handle < 1 || handle > FV_CDP_MAX_SERVERS) return -1;
  pthread_mutex_lock(&g_cdp_mu);
  CdpServer *srv = &g_servers[handle - 1];
  int32_t port = srv->in_use ? srv->port : -1;
  pthread_mutex_unlock(&g_cdp_mu);
  return port;
}

int32_t fv_cdp_set_target_meta(int32_t handle,
                               const void *title, int32_t title_len,
                               const void *url, int32_t url_len) {
  if (handle < 1 || handle > FV_CDP_MAX_SERVERS) return -1;
  pthread_mutex_lock(&g_cdp_mu);
  CdpServer *srv = &g_servers[handle - 1];
  if (!srv->in_use) {
    pthread_mutex_unlock(&g_cdp_mu);
    return -1;
  }
  if (title && title_len >= 0) {
    size_t n = (size_t)title_len;
    if (n >= sizeof(srv->title)) n = sizeof(srv->title) - 1;
    memcpy(srv->title, title, n);
    srv->title[n] = '\0';
  }
  if (url && url_len >= 0) {
    size_t n = (size_t)url_len;
    if (n >= sizeof(srv->url)) n = sizeof(srv->url) - 1;
    memcpy(srv->url, url, n);
    srv->url[n] = '\0';
  }
  pthread_mutex_unlock(&g_cdp_mu);
  return 0;
}

int32_t fv_cdp_poll(int32_t handle, void *out_buf, int32_t cap, int32_t *out_client_id) {
  if (handle < 1 || handle > FV_CDP_MAX_SERVERS) return -1;
  if (out_client_id) *out_client_id = 0;

  pthread_mutex_lock(&g_cdp_mu);
  CdpServer *srv = &g_servers[handle - 1];
  if (!srv->in_use || srv->listen_fd < 0) {
    pthread_mutex_unlock(&g_cdp_mu);
    return -1;
  }

  /* 1. Accept any pending loopback connections */
  for (;;) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    fv_sock_t cs = accept(fv_sock_from_i32(srv->listen_fd), (struct sockaddr *)&peer, &plen);
    if (cs == FV_SOCK_INVALID) break;
    int cfd = fv_sock_to_i32(cs);
    /* Defense-in-depth: verify peer is 127.0.0.1 */
    if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
      fv_sock_close(cfd);
      continue;
    }
    set_nonblock(cfd);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
    int cslot = -1;
    for (int i = 0; i < FV_CDP_MAX_CLIENTS; i++) {
      if (srv->clients[i].fd < 0) {
        cslot = i;
        break;
      }
    }
    if (cslot < 0) {
      fv_sock_close(cfd);
      continue;
    }
    srv->clients[cslot].fd = cfd;
    srv->clients[cslot].id = srv->next_client_id++;
    srv->clients[cslot].upgraded = 0;
    srv->clients[cslot].rx_len = 0;
  }

  /* 2. Read available bytes from clients & process HTTP / WS frames */
  uint8_t tmp[8192];
  for (int i = 0; i < FV_CDP_MAX_CLIENTS; i++) {
    CdpClient *c = &srv->clients[i];
    if (c->fd < 0) continue;

    for (;;) {
      ssize_t n = fv_sock_recv(c->fd, tmp, sizeof(tmp), 0);
      if (n > 0) {
        if (client_append_rx(c, tmp, (size_t)n) < 0) {
          client_reset(c);
          break;
        }
        continue;
      }
      if (n == 0) {
        /* Peer closed */
        if (c->rx_len == 0) {
          client_reset(c);
        }
        break;
      }
      if (fv_sock_eintr()) continue;
      if (fv_sock_wouldblock()) break;
      client_reset(c);
      break;
    }

    if (c->fd < 0) continue;

    if (!c->upgraded) {
      int should_close = handle_http_request(srv, c);
      if (should_close) {
        client_reset(c);
        continue;
      }
    }

    if (c->upgraded && c->rx_len > 0) {
      int r = try_read_ws_frame(c, out_buf, cap);
      if (r > 0) {
        if (out_client_id) *out_client_id = c->id;
        pthread_mutex_unlock(&g_cdp_mu);
        return r;
      }
      if (r < 0) {
        client_reset(c);
        continue;
      }
    }
  }

  pthread_mutex_unlock(&g_cdp_mu);
  return 0;
}

int32_t fv_cdp_send(int32_t handle, int32_t client_id, const void *json, int32_t len) {
  if (handle < 1 || handle > FV_CDP_MAX_SERVERS || len < 0 || (!json && len > 0)) {
    return -1;
  }
  pthread_mutex_lock(&g_cdp_mu);
  CdpServer *srv = &g_servers[handle - 1];
  if (!srv->in_use) {
    pthread_mutex_unlock(&g_cdp_mu);
    return -1;
  }
  int ret = -1;
  for (int i = 0; i < FV_CDP_MAX_CLIENTS; i++) {
    CdpClient *c = &srv->clients[i];
    if (c->fd >= 0 && c->upgraded && c->id == client_id) {
      ret = send_ws_frame(c->fd, 0x1u, (const uint8_t *)json, (size_t)len);
      if (ret < 0) client_reset(c);
      break;
    }
  }
  pthread_mutex_unlock(&g_cdp_mu);
  return ret;
}

void fv_cdp_close(int32_t handle) {
  if (handle < 1 || handle > FV_CDP_MAX_SERVERS) return;
  pthread_mutex_lock(&g_cdp_mu);
  CdpServer *srv = &g_servers[handle - 1];
  if (srv->in_use) {
    for (int i = 0; i < FV_CDP_MAX_CLIENTS; i++) {
      client_reset(&srv->clients[i]);
    }
    if (srv->listen_fd >= 0) {
      fv_sock_close(srv->listen_fd);
      srv->listen_fd = -1;
    }
    srv->in_use = 0;
    srv->port = 0;
  }
  pthread_mutex_unlock(&g_cdp_mu);
}

int32_t fv_cdp_active_count(void) {
  pthread_mutex_lock(&g_cdp_mu);
  int32_t count = 0;
  for (int i = 0; i < FV_CDP_MAX_SERVERS; i++) {
    if (g_servers[i].in_use) {
      count++;
      for (int j = 0; j < FV_CDP_MAX_CLIENTS; j++) {
        if (g_servers[i].clients[j].fd >= 0) count++;
      }
    }
  }
  pthread_mutex_unlock(&g_cdp_mu);
  return count;
}

/* -------------------- Loopback Test Client Helpers -------------------- */

int32_t fv_cdp_client_connect(int32_t port) {
  if (port <= 0 || port > 65535) return -1;
  fv_win_wsa_init();
  fv_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == FV_SOCK_INVALID) return -1;
  int fd = fv_sock_to_i32(s);
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&yes, sizeof(yes));
#endif
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fv_sock_close(fd);
    return -1;
  }
  set_nonblock(fd);
  return fd;
}

int32_t fv_cdp_client_send_raw(int32_t fd, const void *data, int32_t len) {
  if (fd < 0 || len < 0 || (!data && len > 0)) return -1;
  return send_all_nonblock(fd, (const uint8_t *)data, (size_t)len);
}

int32_t fv_cdp_client_recv_raw(int32_t fd, void *out_buf, int32_t cap, int32_t timeout_ms) {
  if (fd < 0 || !out_buf || cap <= 0) return -1;
#ifdef _WIN32
  WSAPOLLFD pfd;
  pfd.fd = fv_sock_from_i32(fd);
  pfd.events = POLLIN;
  pfd.revents = 0;
  int pr = WSAPoll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 1);
#else
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int pr = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 1);
#endif
  if (pr <= 0) return 0;
  ssize_t n = fv_sock_recv(fd, out_buf, (size_t)cap, 0);
  if (n < 0) {
    if (fv_sock_wouldblock()) return 0;
    return -1;
  }
  return (int32_t)n;
}

void fv_cdp_client_close(int32_t fd) {
  if (fd >= 0) fv_sock_close(fd);
}
