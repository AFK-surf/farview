// Native WebSocket (ws:// and wss://) on top of curl-impersonate's curl_ws_* API.
// Non-blocking handshake via curl_multi, non-blocking frame send/recv with fragment reassembly,
// automatic Pong replies, and active socket fd export for 0ms poll wakeup in the host event loop.
#include <curl/curl.h>
#include <curl/websockets.h>
#include <moonbit.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../win_compat.h"

CURL_EXTERN CURLcode curl_easy_impersonate(CURL* curl, const char* target, int default_headers);
extern CURLSH* fv_the_share(void);
extern void fv_apply_connect_to(CURL* c);

typedef struct { char* p; size_t n; size_t cap; } WBuf;

static void wbuf_append(WBuf* b, const void* d, size_t k) {
  if (k == 0) return;
  if (b->n + k > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->n + k) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->n, d, k);
  b->n += k;
}

typedef struct WsTxFrame {
  unsigned char* data;
  size_t len;
  size_t sent;
  unsigned int flags;
  struct WsTxFrame* next;
} WsTxFrame;

enum {
  WS_CONNECTING = 0,
  WS_OPEN = 1,
  WS_CLOSING = 2,
};

typedef struct {
  int32_t id;
  CURL* c;
  struct curl_slist* hs;
  int state;
  int sock;
  WBuf hdrs;
  WBuf rx;
  int rx_active;
  int rx_bin;
  WsTxFrame* tx_head;
  WsTxFrame* tx_tail;
} WsConn;

static CURLM* wsmulti = 0;
static WsConn** ws_list = 0;
static int nws = 0, capws = 0;
static int32_t ws_next_id = 1;
static WBuf ws_ev = {0, 0, 0};

static void wev_u8(unsigned char v) { wbuf_append(&ws_ev, &v, 1); }
static void wev_u32(size_t v) {
  unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
  wbuf_append(&ws_ev, b, 4);
}
static void wev_bytes(const void* p, size_t n) {
  wev_u32(n);
  if (n) wbuf_append(&ws_ev, p, n);
}

// Event tags:
// 1 = OPEN(id, protocol)
// 2 = MSG_TEXT(id, bytes)
// 3 = MSG_BIN(id, bytes)
// 4 = CLOSE(id, code, was_clean, reason)
// 5 = ERROR(id)
static void emit_ws_open(int32_t id, const char* proto) {
  wev_u8(1);
  wev_u32((size_t)(uint32_t)id);
  wev_bytes(proto ? proto : "", proto ? strlen(proto) : 0);
}
static void emit_ws_msg(int32_t id, int is_bin, const void* data, size_t len) {
  wev_u8(is_bin ? 3 : 2);
  wev_u32((size_t)(uint32_t)id);
  wev_bytes(data, len);
}
static void emit_ws_close(int32_t id, int code, int clean, const char* reason, size_t rlen) {
  wev_u8(4);
  wev_u32((size_t)(uint32_t)id);
  wev_u32((size_t)(uint32_t)code);
  wev_u8(clean ? 1 : 0);
  wev_bytes(reason ? reason : "", rlen);
}
static void emit_ws_error(int32_t id) {
  wev_u8(5);
  wev_u32((size_t)(uint32_t)id);
}

static size_t ws_take_header(char* d, size_t s, size_t m, void* u) {
  WsConn* w = (WsConn*)u;
  size_t k = s * m;
  wbuf_append(&w->hdrs, d, k);
  return k;
}

static void ws_extract_proto(WsConn* w, char* out, size_t out_sz) {
  out[0] = 0;
  if (!w->hdrs.p || w->hdrs.n == 0) return;
  const char* p = w->hdrs.p;
  const char* end = p + w->hdrs.n;
  while (p < end) {
    const char* nl = memchr(p, '\n', (size_t)(end - p));
    const char* le = nl ? nl : end;
    size_t len = (size_t)(le - p);
    if (len > 0 && p[len - 1] == '\r') len--;
    const char* key = "sec-websocket-protocol:";
    size_t klen = strlen(key);
    if (len >= klen && strncasecmp(p, key, klen) == 0) {
      const char* v = p + klen;
      const char* ve = p + len;
      while (v < ve && (*v == ' ' || *v == '\t')) v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      size_t vn = (size_t)(ve - v);
      if (vn >= out_sz) vn = out_sz - 1;
      memcpy(out, v, vn);
      out[vn] = 0;
      return;
    }
    p = nl ? nl + 1 : end;
  }
}

static void ws_free_conn(WsConn* w) {
  if (wsmulti && w->c) {
    curl_multi_remove_handle(wsmulti, w->c);
  }
  if (w->c) curl_easy_cleanup(w->c);
  if (w->hs) curl_slist_free_all(w->hs);
  free(w->hdrs.p);
  free(w->rx.p);
  WsTxFrame* f = w->tx_head;
  while (f) {
    WsTxFrame* next = f->next;
    free(f->data);
    free(f);
    f = next;
  }
  free(w);
}

static void ws_remove_at(int idx) {
  ws_free_conn(ws_list[idx]);
  ws_list[idx] = ws_list[--nws];
}

static void ws_queue_tx(WsConn* w, const void* data, size_t len, unsigned int flags) {
  WsTxFrame* f = calloc(1, sizeof(WsTxFrame));
  if (len > 0) {
    f->data = malloc(len);
    memcpy(f->data, data, len);
  }
  f->len = len;
  f->sent = 0;
  f->flags = flags;
  if (w->tx_tail) w->tx_tail->next = f;
  else w->tx_head = f;
  w->tx_tail = f;
}

static int ws_flush_tx(WsConn* w) {
  while (w->tx_head) {
    WsTxFrame* f = w->tx_head;
    size_t sent = 0;
    size_t rem = f->len - f->sent;
    unsigned int fl = f->flags;
    curl_off_t fragsize = 0;
    if ((fl == CURLWS_TEXT || fl == CURLWS_BINARY) && f->len > 0) {
      fl |= CURLWS_OFFSET;
      if (f->sent == 0) fragsize = (curl_off_t)f->len;
    }
    CURLcode rc = curl_ws_send(w->c, rem > 0 ? (const void*)(f->data + f->sent) : (const void*)"", rem, &sent, fragsize, fl);
    if (rc == CURLE_AGAIN) return 1;
    if (rc != CURLE_OK) {
      if (getenv("FV_FETCH_LOG")) fprintf(stderr, "ws_send failed rc=%d (%s) rem=%zu frag=%lld fl=%u\n", (int)rc, curl_easy_strerror(rc), rem, (long long)fragsize, fl);
      return 0;
    }
    f->sent += sent;
    if (f->sent >= f->len) {
      w->tx_head = f->next;
      if (!w->tx_head) w->tx_tail = 0;
      free(f->data);
      free(f);
    } else {
      return 1;
    }
  }
  return 1;
}

int32_t fv_ws_open(moonbit_bytes_t url, moonbit_bytes_t origin, moonbit_bytes_t protocols) {
  if (!wsmulti) wsmulti = curl_multi_init();
  if (nws == capws) {
    capws = capws ? capws * 2 : 8;
    ws_list = realloc(ws_list, capws * sizeof(WsConn*));
  }
  WsConn* w = calloc(1, sizeof(WsConn));
  ws_list[nws++] = w;
  w->id = ws_next_id++;
  w->state = WS_CONNECTING;
  w->sock = -1;
  w->c = curl_easy_init();

  curl_easy_setopt(w->c, CURLOPT_SHARE, fv_the_share());
  const char* jar = getenv("FV_COOKIE_JAR");
  curl_easy_setopt(w->c, CURLOPT_COOKIEFILE, jar ? jar : "");
  if (jar) curl_easy_setopt(w->c, CURLOPT_COOKIEJAR, jar);
  curl_easy_setopt(w->c, CURLOPT_URL, (const char*)url);
  curl_easy_setopt(w->c, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(w->c, CURLOPT_CONNECTTIMEOUT, 15L);
  // Chrome TLS fingerprint without HTTP/2 ALPN or conflicting default headers
  curl_easy_impersonate(w->c, "chrome152", 0);
  curl_easy_setopt(w->c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  fv_apply_connect_to(w->c);

  struct curl_slist* hs = 0;
  const char* orig = (const char*)origin;
  if (orig && *orig) {
    size_t n = strlen(orig) + 16;
    char* line = malloc(n);
    snprintf(line, n, "Origin: %s", orig);
    hs = curl_slist_append(hs, line);
    free(line);
  }
  const char* prot = (const char*)protocols;
  if (prot && *prot) {
    size_t n = strlen(prot) + 32;
    char* line = malloc(n);
    snprintf(line, n, "Sec-WebSocket-Protocol: %s", prot);
    hs = curl_slist_append(hs, line);
    free(line);
  }
  w->hs = hs;
  if (hs) curl_easy_setopt(w->c, CURLOPT_HTTPHEADER, hs);

  curl_easy_setopt(w->c, CURLOPT_HEADERFUNCTION, ws_take_header);
  curl_easy_setopt(w->c, CURLOPT_HEADERDATA, w);
  curl_easy_setopt(w->c, CURLOPT_PRIVATE, (void*)(intptr_t)w->id);

  curl_multi_add_handle(wsmulti, w->c);
  int running = 0;
  curl_multi_perform(wsmulti, &running);
  return w->id;
}

int32_t fv_ws_send(int32_t id, int32_t is_bin, moonbit_bytes_t data, int32_t len) {
  for (int i = 0; i < nws; i++) {
    WsConn* w = ws_list[i];
    if (w->id == id) {
      if (w->state != WS_OPEN) return 0;
      ws_queue_tx(w, (const void*)data, (size_t)(len > 0 ? len : 0), is_bin ? CURLWS_BINARY : CURLWS_TEXT);
      ws_flush_tx(w);
      return 1;
    }
  }
  return 0;
}

void fv_ws_close(int32_t id, int32_t code, moonbit_bytes_t reason, int32_t rlen) {
  for (int i = 0; i < nws; i++) {
    WsConn* w = ws_list[i];
    if (w->id == id) {
      if (w->state == WS_CONNECTING) {
        emit_ws_close(w->id, 1006, 0, "", 0);
        ws_remove_at(i);
        return;
      }
      if (w->state == WS_OPEN) {
        w->state = WS_CLOSING;
        if (code > 0) {
          size_t n = 2 + (size_t)(rlen > 0 ? rlen : 0);
          unsigned char* payload = malloc(n);
          payload[0] = (unsigned char)((code >> 8) & 0xFF);
          payload[1] = (unsigned char)(code & 0xFF);
          if (rlen > 0) memcpy(payload + 2, reason, (size_t)rlen);
          ws_queue_tx(w, payload, n, CURLWS_CLOSE);
          free(payload);
        } else {
          ws_queue_tx(w, "", 0, CURLWS_CLOSE);
        }
        ws_flush_tx(w);
      }
      return;
    }
  }
}

moonbit_bytes_t fv_ws_poll(void) {
  // 1. Drive non-blocking handshakes
  if (wsmulti) {
    int running = 0;
    curl_multi_perform(wsmulti, &running);
    CURLMsg* msg;
    int left = 0;
    while ((msg = curl_multi_info_read(wsmulti, &left))) {
      if (msg->msg != CURLMSG_DONE) continue;
      void* priv = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
      int32_t id = (int32_t)(intptr_t)priv;
      int idx = -1;
      for (int i = 0; i < nws; i++) if (ws_list[i]->id == id) { idx = i; break; }
      if (idx < 0) continue;
      WsConn* w = ws_list[idx];
      long status = 0;
      curl_easy_getinfo(w->c, CURLINFO_RESPONSE_CODE, &status);
      if (msg->data.result == CURLE_OK && status == 101) {
        curl_socket_t s = CURL_SOCKET_BAD;
        curl_easy_getinfo(w->c, CURLINFO_ACTIVESOCKET, &s);
        w->sock = (s != CURL_SOCKET_BAD) ? (int)s : -1;
        if (w->sock >= 0) {
#ifdef _WIN32
          (void)fv_sock_set_nb(w->sock, 1);
#else
          int fl = fcntl(w->sock, F_GETFL, 0);
          if (fl >= 0) fcntl(w->sock, F_SETFL, fl | O_NONBLOCK);
#endif
        }
        w->state = WS_OPEN;
        char proto[256];
        ws_extract_proto(w, proto, sizeof proto);
        emit_ws_open(w->id, proto);
      } else {
        if (getenv("FV_FETCH_LOG")) {
          fprintf(stderr, "ws handshake failed (rc=%d status=%ld)\n", (int)msg->data.result, status);
        }
        emit_ws_error(w->id);
        emit_ws_close(w->id, 1006, 0, "", 0);
        ws_remove_at(idx);
      }
    }
  }

  // 2. Poll OPEN / CLOSING connections for tx flush & rx frames
  unsigned char buf[16384];
  int i = 0;
  while (i < nws) {
    WsConn* w = ws_list[i];
    if (w->state == WS_CONNECTING) { i++; continue; }

    if (!ws_flush_tx(w)) {
      emit_ws_error(w->id);
      emit_ws_close(w->id, 1006, 0, "", 0);
      ws_remove_at(i);
      continue;
    }

    int closed = 0;
    for (int iter = 0; iter < 64; iter++) {
      size_t rlen = 0;
      const struct curl_ws_frame* meta = 0;
      CURLcode rc = curl_ws_recv(w->c, buf, sizeof buf, &rlen, &meta);
      if (rc == CURLE_AGAIN) break;
      if (rc != CURLE_OK || !meta) {
        if (getenv("FV_FETCH_LOG")) fprintf(stderr, "ws_recv failed rc=%d (%s) meta=%p\n", (int)rc, curl_easy_strerror(rc), (const void*)meta);
        emit_ws_close(w->id, 1006, 0, "", 0);
        closed = 1;
        break;
      }
      if (rlen == 0 && meta->flags == 0) {
        emit_ws_close(w->id, 1006, 0, "", 0);
        closed = 1;
        break;
      }
      if (meta->flags & CURLWS_PING) {
        size_t sent = 0;
        curl_ws_send(w->c, buf, rlen, &sent, 0, CURLWS_PONG);
        continue;
      }
      if (meta->flags & CURLWS_PONG) {
        continue;
      }
      if (meta->flags & CURLWS_CLOSE) {
        int code = 1005;
        const char* reason = "";
        size_t reason_len = 0;
        if (rlen >= 2) {
          code = ((int)buf[0] << 8) | (int)buf[1];
          reason = (const char*)(buf + 2);
          reason_len = rlen - 2;
        }
        if (w->state == WS_OPEN) {
          size_t sent = 0;
          curl_ws_send(w->c, buf, rlen >= 2 ? 2 : 0, &sent, 0, CURLWS_CLOSE);
        }
        emit_ws_close(w->id, code, 1, reason, reason_len);
        closed = 1;
        break;
      }
      if (meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) {
        if (!w->rx_active) {
          w->rx_active = 1;
          w->rx_bin = (meta->flags & CURLWS_BINARY) ? 1 : 0;
          w->rx.n = 0;
        }
        wbuf_append(&w->rx, buf, rlen);
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
          emit_ws_msg(w->id, w->rx_bin, w->rx.p, w->rx.n);
          w->rx.n = 0;
          w->rx_active = 0;
        }
      }
    }
    if (closed) {
      ws_remove_at(i);
    } else {
      i++;
    }
  }

  moonbit_bytes_t out = moonbit_make_bytes(ws_ev.n, 0);
  if (ws_ev.n) memcpy(out, ws_ev.p, ws_ev.n);
  ws_ev.n = 0;
  return out;
}

int32_t fv_ws_active(void) { return nws; }

moonbit_bytes_t fv_ws_fds(void) {
  int count = 0;
  for (int i = 0; i < nws; i++) {
    if (ws_list[i]->state != WS_CONNECTING && ws_list[i]->sock >= 0) count++;
  }
  moonbit_bytes_t out = moonbit_make_bytes(count * 4, 0);
  unsigned char* o = (unsigned char*)out;
  for (int i = 0; i < nws; i++) {
    if (ws_list[i]->state != WS_CONNECTING && ws_list[i]->sock >= 0) {
      uint32_t fd = (uint32_t)ws_list[i]->sock;
      *o++ = (unsigned char)(fd >> 24);
      *o++ = (unsigned char)(fd >> 16);
      *o++ = (unsigned char)(fd >> 8);
      *o++ = (unsigned char)fd;
    }
  }
  return out;
}

void fv_ws_clear(void) {
  while (nws > 0) ws_remove_at(0);
  free(ws_ev.p);
  ws_ev.p = 0;
  ws_ev.cap = 0;
  ws_ev.n = 0;
}
