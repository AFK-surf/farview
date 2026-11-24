/*
 * Farview Wave R1: Zero-dependency native WebRTC transport & media shim.
 * - Non-blocking POSIX UDP socket pool + local IPv4 discovery
 * - Real RFC 5764 BoringSSL DTLS 1.2 (Sans-IO memory BIOs) + ECDSA P-256 self-signed X.509
 *   + constant-time SHA-256 peer fingerprint verification + SRTP keying material export
 *   + encrypted DTLS application channel (RTCDataChannel)
 * - Safe OS audio/video/display capture device enumeration & deterministic frame pipeline
 *
 * BoringSSL symbols are already statically linked in deps/curl-impersonate/libcurl-impersonate.a.
 * Following native/js/crypto.cc, we declare minimal opaque-pointer prototypes without external headers.
 */

#ifdef __APPLE__
__asm__(".linker_option \"-framework\", \"Security\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n");
#endif

#include "webrtc_native.h"
#include "av_macos.h"
#include "audio_out.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Minimal opaque BoringSSL declarations (linked from libcurl-impersonate.a)
 * -------------------------------------------------------------------------- */

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
typedef struct evp_md_st EVP_MD;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct ec_key_st EC_KEY;
typedef struct x509_st X509;
typedef struct X509_name_st X509_NAME;
typedef struct asn1_string_st ASN1_INTEGER;
typedef struct asn1_string_st ASN1_TIME;
typedef struct x509_store_ctx_st X509_STORE_CTX;

typedef struct srtp_protection_profile_st {
  const char *name;
  unsigned long id;
} SRTP_PROTECTION_PROFILE;

extern const SSL_METHOD *DTLS_method(void);
extern SSL_CTX *SSL_CTX_new(const SSL_METHOD *method);
extern void SSL_CTX_free(SSL_CTX *ctx);
extern int SSL_CTX_set_tlsext_use_srtp(SSL_CTX *ctx, const char *profiles);
extern void SSL_CTX_set_verify(SSL_CTX *ctx, int mode,
                               int (*cb)(int, X509_STORE_CTX *));
extern int SSL_CTX_use_certificate(SSL_CTX *ctx, X509 *x);
extern int SSL_CTX_use_PrivateKey(SSL_CTX *ctx, EVP_PKEY *pkey);

extern SSL *SSL_new(SSL_CTX *ctx);
extern void SSL_free(SSL *ssl);
extern void SSL_set_bio(SSL *ssl, BIO *rbio, BIO *wbio);
extern void SSL_set_connect_state(SSL *ssl);
extern void SSL_set_accept_state(SSL *ssl);
extern int SSL_do_handshake(SSL *ssl);
extern int DTLSv1_handle_timeout(SSL *ssl);
extern void DTLSv1_set_initial_timeout_duration(SSL *ssl, unsigned duration_ms);
extern int SSL_is_init_finished(const SSL *ssl);
extern int SSL_read(SSL *ssl, void *buf, int num);
extern int SSL_write(SSL *ssl, const void *buf, int num);
extern int SSL_get_error(const SSL *ssl, int ret);
extern X509 *SSL_get_peer_certificate(const SSL *ssl);
extern int SSL_export_keying_material(SSL *ssl, uint8_t *out, size_t out_len,
                                      const char *label, size_t label_len,
                                      const uint8_t *context, size_t context_len,
                                      int use_context);
extern const SRTP_PROTECTION_PROFILE *SSL_get_selected_srtp_profile(SSL *ssl);

extern BIO *BIO_new(const BIO_METHOD *type);
extern const BIO_METHOD *BIO_s_mem(void);
extern int BIO_write(BIO *b, const void *data, int len);
extern int BIO_read(BIO *b, void *data, int len);
extern size_t BIO_ctrl_pending(const BIO *b);

extern EC_KEY *EC_KEY_new_by_curve_name(int nid);
extern int EC_KEY_generate_key(EC_KEY *key);
extern EVP_PKEY *EVP_PKEY_new(void);
extern int EVP_PKEY_assign_EC_KEY(EVP_PKEY *pkey, EC_KEY *key);
extern void EVP_PKEY_free(EVP_PKEY *pkey);

extern X509 *X509_new(void);
extern void X509_free(X509 *x);
extern int X509_set_version(X509 *x, long version);
extern ASN1_INTEGER *X509_get_serialNumber(X509 *x);
extern int ASN1_INTEGER_set(ASN1_INTEGER *a, long v);
extern ASN1_TIME *X509_get_notBefore(const X509 *x);
extern ASN1_TIME *X509_get_notAfter(const X509 *x);
extern ASN1_TIME *X509_gmtime_adj(ASN1_TIME *s, long adj);
extern int X509_set_pubkey(X509 *x, EVP_PKEY *pkey);
extern X509_NAME *X509_get_subject_name(const X509 *x);
extern int X509_NAME_add_entry_by_txt(X509_NAME *name, const char *field,
                                      int type, const uint8_t *bytes, int len,
                                      int loc, int set);
extern int X509_set_issuer_name(X509 *x, X509_NAME *name);
extern int X509_sign(X509 *x, EVP_PKEY *pkey, const EVP_MD *md);
extern int X509_digest(const X509 *data, const EVP_MD *type, uint8_t *md,
                       unsigned int *len);

extern const EVP_MD *EVP_sha256(void);
extern int RAND_bytes(uint8_t *buf, size_t len);

/* BoringSSL constants */
#define FV_NID_X9_62_PRIME256V1 415
#define FV_SSL_VERIFY_PEER 0x01
#define FV_SSL_VERIFY_FAIL_IF_NO_PEER_CERT 0x02
#define FV_SSL_ERROR_WANT_READ 2
#define FV_SSL_ERROR_WANT_WRITE 3
#define FV_MBSTRING_ASC 0x1001

/* First DTLS flight retransmit delay (doubles per attempt); the RFC 6347 default of 1 s stalls setup on one loss. */
#define FV_DTLS_INITIAL_TIMEOUT_MS 50

/* --------------------------------------------------------------------------
 * 1. Non-blocking POSIX UDP socket pool
 * -------------------------------------------------------------------------- */

#define MAX_UDP_SLOTS 128

typedef struct {
  int used;
  int fd;
  int32_t port;
} UdpSlot;

static pthread_mutex_t g_udp_mu = PTHREAD_MUTEX_INITIALIZER;
static UdpSlot g_udp_slots[MAX_UDP_SLOTS];

int32_t fv_webrtc_udp_open(int32_t loopback_only) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
  addr.sin_port = htons(0);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  socklen_t alen = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &alen) < 0) {
    close(fd);
    return -1;
  }
  int32_t bound_port = (int32_t)ntohs(addr.sin_port);

  pthread_mutex_lock(&g_udp_mu);
  int32_t handle = -1;
  for (int i = 0; i < MAX_UDP_SLOTS; i++) {
    if (!g_udp_slots[i].used) {
      g_udp_slots[i].used = 1;
      g_udp_slots[i].fd = fd;
      g_udp_slots[i].port = bound_port;
      handle = i + 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_udp_mu);

  if (handle < 0) {
    close(fd);
    return -1;
  }
  return handle;
}

int32_t fv_webrtc_udp_port(int32_t handle) {
  if (handle < 1 || handle > MAX_UDP_SLOTS) return -1;
  pthread_mutex_lock(&g_udp_mu);
  int32_t port = g_udp_slots[handle - 1].used ? g_udp_slots[handle - 1].port : -1;
  pthread_mutex_unlock(&g_udp_mu);
  return port;
}

int32_t fv_webrtc_udp_send(int32_t handle, const char *ip, int32_t port,
                           const uint8_t *data, int32_t len) {
  if (handle < 1 || handle > MAX_UDP_SLOTS || !ip || !data) return -1;
  if (port < 1 || port > 65535 || len < 0 || len > 65507) return -1;

  pthread_mutex_lock(&g_udp_mu);
  int fd = g_udp_slots[handle - 1].used ? g_udp_slots[handle - 1].fd : -1;
  pthread_mutex_unlock(&g_udp_mu);
  if (fd < 0) return -1;

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) return -1;

  ssize_t n = sendto(fd, data, (size_t)len, 0, (struct sockaddr *)&dst, sizeof(dst));
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
  return (int32_t)n;
}

int32_t fv_webrtc_udp_recv(int32_t handle, uint8_t *out_buf, int32_t cap,
                           char *out_ip, int32_t ip_cap, int32_t *out_port) {
  if (handle < 1 || handle > MAX_UDP_SLOTS || !out_buf || cap <= 0) return -1;

  pthread_mutex_lock(&g_udp_mu);
  int fd = g_udp_slots[handle - 1].used ? g_udp_slots[handle - 1].fd : -1;
  pthread_mutex_unlock(&g_udp_mu);
  if (fd < 0) return -1;

  struct sockaddr_in src;
  socklen_t slen = sizeof(src);
  memset(&src, 0, sizeof(src));

  ssize_t n = recvfrom(fd, out_buf, (size_t)cap, 0, (struct sockaddr *)&src, &slen);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }

  if (out_ip && ip_cap > 0) {
    char tmp[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &src.sin_addr, tmp, sizeof(tmp))) {
      snprintf(out_ip, (size_t)ip_cap, "%s", tmp);
    } else {
      out_ip[0] = '\0';
    }
  }
  if (out_port) {
    *out_port = (int32_t)ntohs(src.sin_port);
  }
  return (int32_t)n;
}

void fv_webrtc_udp_close(int32_t handle) {
  if (handle < 1 || handle > MAX_UDP_SLOTS) return;
  pthread_mutex_lock(&g_udp_mu);
  int fd = -1;
  if (g_udp_slots[handle - 1].used) {
    fd = g_udp_slots[handle - 1].fd;
    g_udp_slots[handle - 1].used = 0;
    g_udp_slots[handle - 1].fd = -1;
    g_udp_slots[handle - 1].port = 0;
  }
  pthread_mutex_unlock(&g_udp_mu);
  if (fd >= 0) close(fd);
}

int32_t fv_webrtc_udp_active_count(void) {
  pthread_mutex_lock(&g_udp_mu);
  int32_t cnt = 0;
  for (int i = 0; i < MAX_UDP_SLOTS; i++) {
    if (g_udp_slots[i].used) cnt++;
  }
  pthread_mutex_unlock(&g_udp_mu);
  return cnt;
}

int32_t fv_webrtc_udp_wait(int32_t handle_a, int32_t handle_b, int32_t timeout_ms) {
  const int32_t handles[2] = {handle_a, handle_b};
  struct pollfd fds[2];
  nfds_t count = 0;
  pthread_mutex_lock(&g_udp_mu);
  for (int i = 0; i < 2; i++) {
    int32_t h = handles[i];
    if (h < 1 || h > MAX_UDP_SLOTS || !g_udp_slots[h - 1].used) continue;
    fds[count].fd = g_udp_slots[h - 1].fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    count++;
  }
  pthread_mutex_unlock(&g_udp_mu);
  if (count == 0) return -1;
  return (int32_t)poll(fds, count, timeout_ms < 0 ? 0 : timeout_ms);
}

int64_t fv_webrtc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int32_t fv_webrtc_local_ips(char *out_buf, int32_t max_len) {
  if (!out_buf || max_len <= 1) return -1;
  out_buf[0] = '\0';

  struct ifaddrs *ifas = NULL;
  int32_t pos = 0;
  if (getifaddrs(&ifas) == 0 && ifas) {
    for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      if (!(ifa->ifa_flags & IFF_UP)) continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) continue;
      struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
      char ip[INET_ADDRSTRLEN];
      if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
      if (strncmp(ip, "169.254.", 8) == 0) continue; /* skip link-local */
      int need = (pos > 0 ? 1 : 0) + (int)strlen(ip);
      if (pos + need + 1 >= max_len) break;
      if (pos > 0) out_buf[pos++] = ',';
      memcpy(out_buf + pos, ip, strlen(ip));
      pos += (int32_t)strlen(ip);
      out_buf[pos] = '\0';
    }
    freeifaddrs(ifas);
  }

  if (pos == 0) {
    const char *fb = "127.0.0.1";
    int n = (int)strlen(fb);
    if (n >= max_len) return -1;
    memcpy(out_buf, fb, (size_t)n + 1);
    pos = n;
  }
  return pos;
}

/* --------------------------------------------------------------------------
 * 2. Real RFC 5764 BoringSSL DTLS 1.2 + SRTP Keying Material Export
 * -------------------------------------------------------------------------- */

#define MAX_DTLS_SLOTS 128

typedef struct {
  int used;
  int is_server;
  SSL_CTX *ctx;
  SSL *ssl;
  BIO *rbio; /* owned by ssl after SSL_set_bio */
  BIO *wbio; /* owned by ssl after SSL_set_bio */
  X509 *cert;
  EVP_PKEY *pkey;
  char local_fp[100]; /* "AA:BB:...:FF" (95 chars + NUL) */
} DtlsSlot;

static pthread_mutex_t g_dtls_mu = PTHREAD_MUTEX_INITIALIZER;
static DtlsSlot g_dtls_slots[MAX_DTLS_SLOTS];

static int always_ok_verify_cb(int preverify_ok, X509_STORE_CTX *ctx) {
  (void)preverify_ok;
  (void)ctx;
  /* Self-signed WebRTC certs are verified strictly by SHA-256 fingerprint match against SDP. */
  return 1;
}

static int format_sha256_fp(const X509 *cert, char *out, size_t cap) {
  if (!cert || !out || cap < 96) return -1;
  uint8_t md[32];
  unsigned int md_len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1 || md_len != 32) {
    return -1;
  }
  static const char hex[] = "0123456789ABCDEF";
  size_t pos = 0;
  for (unsigned int i = 0; i < 32; i++) {
    if (i > 0) out[pos++] = ':';
    out[pos++] = hex[(md[i] >> 4) & 0xF];
    out[pos++] = hex[md[i] & 0xF];
  }
  out[pos] = '\0';
  return (int)pos;
}

static int generate_ephemeral_ecdsa_cert(EVP_PKEY **out_pkey, X509 **out_cert,
                                         char *out_fp, size_t fp_cap) {
  EC_KEY *ec = EC_KEY_new_by_curve_name(FV_NID_X9_62_PRIME256V1);
  if (!ec) return -1;
  if (EC_KEY_generate_key(ec) != 1) {
    /* EVP_PKEY_assign_EC_KEY not called yet; avoid leaking if possible */
    EVP_PKEY *tmp = EVP_PKEY_new();
    if (tmp) {
      if (EVP_PKEY_assign_EC_KEY(tmp, ec) == 1) EVP_PKEY_free(tmp);
    }
    return -1;
  }
  EVP_PKEY *pkey = EVP_PKEY_new();
  if (!pkey) return -1;
  if (EVP_PKEY_assign_EC_KEY(pkey, ec) != 1) {
    EVP_PKEY_free(pkey);
    return -1;
  }

  X509 *cert = X509_new();
  if (!cert) {
    EVP_PKEY_free(pkey);
    return -1;
  }

  uint32_t serial_raw = 1;
  (void)RAND_bytes((uint8_t *)&serial_raw, sizeof(serial_raw));
  long serial = (long)(serial_raw & 0x7FFFFFFFu);
  if (serial == 0) serial = 1;

  if (X509_set_version(cert, 2) != 1) { X509_free(cert); EVP_PKEY_free(pkey); return -11; }
  if (ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) != 1) { X509_free(cert); EVP_PKEY_free(pkey); return -12; }
  if (X509_gmtime_adj(X509_get_notBefore(cert), -86400L) == NULL) { X509_free(cert); EVP_PKEY_free(pkey); return -13; }
  if (X509_gmtime_adj(X509_get_notAfter(cert), 30L * 86400L) == NULL) { X509_free(cert); EVP_PKEY_free(pkey); return -14; }
  if (X509_set_pubkey(cert, pkey) != 1) { X509_free(cert); EVP_PKEY_free(pkey); return -15; }

  X509_NAME *name = X509_get_subject_name(cert);
  if (!name) { X509_free(cert); EVP_PKEY_free(pkey); return -16; }
  if (X509_NAME_add_entry_by_txt(name, "CN", 12 /* V_ASN1_UTF8STRING */,
                                 (const uint8_t *)"Farview-WebRTC", 14, -1, 0) != 1) {
    X509_free(cert); EVP_PKEY_free(pkey); return -17;
  }
  if (X509_set_issuer_name(cert, name) != 1) { X509_free(cert); EVP_PKEY_free(pkey); return -18; }
  if (X509_sign(cert, pkey, EVP_sha256()) <= 0) { X509_free(cert); EVP_PKEY_free(pkey); return -19; }
  if (format_sha256_fp(cert, out_fp, fp_cap) != 95) { X509_free(cert); EVP_PKEY_free(pkey); return -20; }

  *out_pkey = pkey;
  *out_cert = cert;
  return 0;
}

int32_t fv_dtls_new(int32_t is_server) {
  EVP_PKEY *pkey = NULL;
  X509 *cert = NULL;
  char fp[100];
  memset(fp, 0, sizeof(fp));

  int cert_rc = generate_ephemeral_ecdsa_cert(&pkey, &cert, fp, sizeof(fp));
  if (cert_rc != 0) {
    return cert_rc;
  }

  SSL_CTX *ctx = SSL_CTX_new(DTLS_method());
  if (!ctx) {
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -21;
  }

  if (SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80") != 0) {
    SSL_CTX_free(ctx); X509_free(cert); EVP_PKEY_free(pkey); return -22;
  }
  if (SSL_CTX_use_certificate(ctx, cert) != 1) {
    SSL_CTX_free(ctx); X509_free(cert); EVP_PKEY_free(pkey); return -23;
  }
  if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
    SSL_CTX_free(ctx); X509_free(cert); EVP_PKEY_free(pkey); return -24;
  }

  SSL_CTX_set_verify(ctx, FV_SSL_VERIFY_PEER | FV_SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     always_ok_verify_cb);

  SSL *ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }

  BIO *rbio = BIO_new(BIO_s_mem());
  BIO *wbio = BIO_new(BIO_s_mem());
  if (!rbio || !wbio) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }
  SSL_set_bio(ssl, rbio, wbio);
  DTLSv1_set_initial_timeout_duration(ssl, FV_DTLS_INITIAL_TIMEOUT_MS);

  if (is_server) {
    SSL_set_accept_state(ssl);
  } else {
    SSL_set_connect_state(ssl);
  }

  pthread_mutex_lock(&g_dtls_mu);
  int32_t handle = -1;
  for (int i = 0; i < MAX_DTLS_SLOTS; i++) {
    if (!g_dtls_slots[i].used) {
      g_dtls_slots[i].used = 1;
      g_dtls_slots[i].is_server = is_server ? 1 : 0;
      g_dtls_slots[i].ctx = ctx;
      g_dtls_slots[i].ssl = ssl;
      g_dtls_slots[i].rbio = rbio;
      g_dtls_slots[i].wbio = wbio;
      g_dtls_slots[i].cert = cert;
      g_dtls_slots[i].pkey = pkey;
      memcpy(g_dtls_slots[i].local_fp, fp, sizeof(fp));
      handle = i + 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);

  if (handle < 0) {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }
  return handle;
}

int32_t fv_dtls_set_role(int32_t handle, int32_t is_server) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    g_dtls_slots[handle - 1].is_server = is_server ? 1 : 0;
    if (is_server) {
      SSL_set_accept_state(g_dtls_slots[handle - 1].ssl);
    } else {
      SSL_set_connect_state(g_dtls_slots[handle - 1].ssl);
    }
    res = 0;
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

void fv_dtls_close(int32_t handle) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS) return;
  SSL *ssl = NULL;
  SSL_CTX *ctx = NULL;
  X509 *cert = NULL;
  EVP_PKEY *pkey = NULL;

  pthread_mutex_lock(&g_dtls_mu);
  if (g_dtls_slots[handle - 1].used) {
    ssl = g_dtls_slots[handle - 1].ssl;
    ctx = g_dtls_slots[handle - 1].ctx;
    cert = g_dtls_slots[handle - 1].cert;
    pkey = g_dtls_slots[handle - 1].pkey;
    memset(&g_dtls_slots[handle - 1], 0, sizeof(DtlsSlot));
  }
  pthread_mutex_unlock(&g_dtls_mu);

  if (ssl) SSL_free(ssl);
  if (ctx) SSL_CTX_free(ctx);
  if (cert) X509_free(cert);
  if (pkey) EVP_PKEY_free(pkey);
}

int32_t fv_dtls_active_count(void) {
  pthread_mutex_lock(&g_dtls_mu);
  int32_t cnt = 0;
  for (int i = 0; i < MAX_DTLS_SLOTS; i++) {
    if (g_dtls_slots[i].used) cnt++;
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return cnt;
}

int32_t fv_dtls_local_fingerprint(int32_t handle, char *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !out_buf || cap <= 0) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used) {
    int32_t n = (int32_t)strlen(g_dtls_slots[handle - 1].local_fp);
    if (n < cap) {
      memcpy(out_buf, g_dtls_slots[handle - 1].local_fp, (size_t)n + 1);
      res = n;
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_feed(int32_t handle, const uint8_t *pkt, int32_t len) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !pkt || len <= 0 || len > 65535) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].rbio) {
    int w = BIO_write(g_dtls_slots[handle - 1].rbio, pkt, len);
    res = (w > 0) ? (int32_t)w : -1;
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_step(int32_t handle) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    SSL *ssl = g_dtls_slots[handle - 1].ssl;
    if (SSL_is_init_finished(ssl)) {
      res = 1;
    } else {
      int r = SSL_do_handshake(ssl);
      if (r == 1) {
        res = 1;
      } else {
        int err = SSL_get_error(ssl, r);
        if (err == FV_SSL_ERROR_WANT_READ || err == FV_SSL_ERROR_WANT_WRITE) {
          /* Memory BIOs have no timer: resend the last flight here once its retransmit timeout expires. */
          res = (DTLSv1_handle_timeout(ssl) < 0) ? -1 : 0;
        } else {
          res = -1;
        }
      }
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_poll_out(int32_t handle, uint8_t *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !out_buf || cap <= 0) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].wbio) {
    BIO *wbio = g_dtls_slots[handle - 1].wbio;
    size_t pending = BIO_ctrl_pending(wbio);
    if (pending == 0) {
      res = 0;
    } else {
      int to_read = (pending > (size_t)cap) ? cap : (int)pending;
      int n = BIO_read(wbio, out_buf, to_read);
      res = (n > 0) ? (int32_t)n : 0;
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_is_connected(int32_t handle) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS) return 0;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t ok = 0;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    ok = SSL_is_init_finished(g_dtls_slots[handle - 1].ssl) ? 1 : 0;
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return ok;
}

int32_t fv_dtls_peer_fingerprint(int32_t handle, char *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !out_buf || cap < 96) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    X509 *peer = SSL_get_peer_certificate(g_dtls_slots[handle - 1].ssl);
    if (peer) {
      res = format_sha256_fp(peer, out_buf, (size_t)cap);
      X509_free(peer);
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

static uint8_t to_upper_ascii(uint8_t c) {
  return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
}

int32_t fv_dtls_verify_peer_fingerprint(int32_t handle, const char *expected_fp,
                                        int32_t expected_len) {
  if (!expected_fp || expected_len != 95) return 0;
  char actual[100];
  int32_t n = fv_dtls_peer_fingerprint(handle, actual, (int32_t)sizeof(actual));
  if (n != 95) return 0;
  /* Constant-time case-insensitive comparison over all 95 bytes */
  uint8_t diff = 0;
  for (int i = 0; i < 95; i++) {
    diff |= (uint8_t)(to_upper_ascii((uint8_t)actual[i]) ^
                      to_upper_ascii((uint8_t)expected_fp[i]));
  }
  return diff == 0 ? 1 : 0;
}

int32_t fv_dtls_export_srtp(int32_t handle, uint8_t *out_buf, int32_t cap,
                            int32_t *out_profile_id) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !out_buf || cap < 60) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    SSL *ssl = g_dtls_slots[handle - 1].ssl;
    if (SSL_is_init_finished(ssl)) {
      const SRTP_PROTECTION_PROFILE *prof = SSL_get_selected_srtp_profile(ssl);
      int32_t pid = 0;
      size_t need = 0;
      if (prof && prof->name) {
        if (strcmp(prof->name, "SRTP_AEAD_AES_128_GCM") == 0 || prof->id == 0x0007ul) {
          pid = FV_SRTP_PROFILE_AEAD_AES_128_GCM;
          need = 56; /* 16+16 keys + 12+12 salts */
        } else if (strcmp(prof->name, "SRTP_AES128_CM_SHA1_80") == 0 || prof->id == 0x0001ul) {
          pid = FV_SRTP_PROFILE_AES128_CM_SHA1_80;
          need = 60; /* 16+16 keys + 14+14 salts */
        }
      }
      if (need > 0 && (size_t)cap >= need) {
        if (SSL_export_keying_material(ssl, out_buf, need,
                                       "EXTRACTOR-dtls_srtp", 19,
                                       NULL, 0, 0) == 1) {
          if (out_profile_id) *out_profile_id = pid;
          res = (int32_t)need;
        }
      }
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_app_write(int32_t handle, const uint8_t *data, int32_t len) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !data || len <= 0 || len > 65535) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    SSL *ssl = g_dtls_slots[handle - 1].ssl;
    if (SSL_is_init_finished(ssl)) {
      int n = SSL_write(ssl, data, len);
      if (n > 0) {
        res = (int32_t)n;
      } else {
        int err = SSL_get_error(ssl, n);
        res = (err == FV_SSL_ERROR_WANT_READ || err == FV_SSL_ERROR_WANT_WRITE) ? 0 : -1;
      }
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

int32_t fv_dtls_app_read(int32_t handle, uint8_t *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_DTLS_SLOTS || !out_buf || cap <= 0) return -1;
  pthread_mutex_lock(&g_dtls_mu);
  int32_t res = -1;
  if (g_dtls_slots[handle - 1].used && g_dtls_slots[handle - 1].ssl) {
    SSL *ssl = g_dtls_slots[handle - 1].ssl;
    if (SSL_is_init_finished(ssl)) {
      int n = SSL_read(ssl, out_buf, cap);
      if (n > 0) {
        res = (int32_t)n;
      } else {
        int err = SSL_get_error(ssl, n);
        res = (err == FV_SSL_ERROR_WANT_READ || err == FV_SSL_ERROR_WANT_WRITE) ? 0 : -1;
      }
    }
  }
  pthread_mutex_unlock(&g_dtls_mu);
  return res;
}

/* --------------------------------------------------------------------------
 * 3. Real OS Audio / Video / Screen Capture Device Enumeration & Frame Source
 *    (macOS AVFoundation / CoreAudio / CoreGraphics via dlopen + explicit fallback)
 * -------------------------------------------------------------------------- */

#define MAX_CAPTURE_SLOTS 64

typedef struct {
  int used;
  int32_t kind;
  int32_t width;
  int32_t height;
  int32_t mode;
  int32_t reason;
  uint32_t seq;
  FvMacCapture *mac_cap;
} CaptureSlot;

static pthread_mutex_t g_cap_mu = PTHREAD_MUTEX_INITIALIZER;
static CaptureSlot g_cap_slots[MAX_CAPTURE_SLOTS];

int32_t fv_capture_enumerate_devices(char *out_buf, int32_t cap) {
  return fv_av_macos_enumerate_devices(out_buf, cap, NULL);
}

int32_t fv_capture_device_summary(char *out_buf, int32_t cap) {
  if (!out_buf || cap <= 1) return -1;
  char tmp[4096];
  FvAvDeviceSummary s;
  memset(&s, 0, sizeof(s));
  fv_av_macos_enumerate_devices(tmp, (int32_t)sizeof(tmp), &s);
  int n = snprintf(
      out_buf, (size_t)cap,
      "avf=%d coreaudio=%d coregraphics=%d sck=%d real_cameras=%d real_mics=%d "
      "real_speakers=%d cam_auth=%d mic_auth=%d screen_tcc=%d",
      s.avfoundation_loaded, s.coreaudio_loaded, s.coregraphics_loaded,
      s.screencapturekit_loaded, s.real_cameras, s.real_mics, s.real_speakers,
      s.cam_auth_status, s.mic_auth_status, s.screen_tcc_granted);
  if (n < 0) return -1;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

int32_t fv_capture_open_ex(int32_t kind, int32_t width, int32_t height, int32_t flags) {
  if (kind < FV_CAPTURE_AUDIO || kind > FV_CAPTURE_SCREEN) return -1;
  if (kind == FV_CAPTURE_AUDIO) {
    width = 48000;
    height = 1;
  } else {
    if (width <= 0 || width > 3840) width = 320;
    if (height <= 0 || height > 2160) height = 240;
  }

  int32_t mode = FV_CAP_MODE_FALLBACK;
  int32_t reason = FV_CAP_REASON_OK;
  FvMacCapture *mc = fv_av_macos_capture_open(kind, width, height, &mode, &reason);
  if (!mc && (flags & FV_CAP_FLAG_STRICT)) {
    if (reason == FV_CAP_REASON_PERMISSION_DENIED || reason == FV_CAP_REASON_TCC_NOT_DETERMINED) {
      return -2; /* explicit permission error */
    }
    if (reason == FV_CAP_REASON_NO_DEVICE) {
      return -3; /* explicit no-device error */
    }
    return -1;
  }

  pthread_mutex_lock(&g_cap_mu);
  int32_t handle = -1;
  for (int i = 0; i < MAX_CAPTURE_SLOTS; i++) {
    if (!g_cap_slots[i].used) {
      g_cap_slots[i].used = 1;
      g_cap_slots[i].kind = kind;
      g_cap_slots[i].width = width;
      g_cap_slots[i].height = height;
      g_cap_slots[i].mode = mc ? mode : FV_CAP_MODE_FALLBACK;
      g_cap_slots[i].reason = mc ? FV_CAP_REASON_OK : reason;
      g_cap_slots[i].seq = 0;
      g_cap_slots[i].mac_cap = mc;
      handle = i + 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_cap_mu);

  if (handle < 0 && mc) {
    fv_av_macos_capture_close(mc);
  }
  return handle;
}

int32_t fv_capture_open(int32_t kind, int32_t width, int32_t height) {
  return fv_capture_open_ex(kind, width, height, FV_CAP_FLAG_ALLOW_FALLBACK);
}

int32_t fv_capture_status(int32_t handle, char *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_CAPTURE_SLOTS || !out_buf || cap <= 1) return -1;
  pthread_mutex_lock(&g_cap_mu);
  if (!g_cap_slots[handle - 1].used) {
    pthread_mutex_unlock(&g_cap_mu);
    return -1;
  }
  int32_t mode = g_cap_slots[handle - 1].mode;
  int32_t reason = g_cap_slots[handle - 1].reason;
  int32_t kind = g_cap_slots[handle - 1].kind;
  uint32_t seq = g_cap_slots[handle - 1].seq;
  pthread_mutex_unlock(&g_cap_mu);

  int n = snprintf(out_buf, (size_t)cap, "kind=%d mode=%s reason=%s seq=%u",
                   kind, fv_cap_mode_name(mode), fv_cap_reason_name(reason), seq);
  if (n < 0) return -1;
  if (n >= cap) n = cap - 1;
  return (int32_t)n;
}

int32_t fv_capture_read_frame(int32_t handle, uint8_t *out_buf, int32_t cap) {
  if (handle < 1 || handle > MAX_CAPTURE_SLOTS || !out_buf || cap <= 0) return -1;

  pthread_mutex_lock(&g_cap_mu);
  if (!g_cap_slots[handle - 1].used) {
    pthread_mutex_unlock(&g_cap_mu);
    return -1;
  }
  int32_t kind = g_cap_slots[handle - 1].kind;
  int32_t w = g_cap_slots[handle - 1].width;
  int32_t h = g_cap_slots[handle - 1].height;
  uint32_t seq = g_cap_slots[handle - 1].seq++;
  FvMacCapture *mc = g_cap_slots[handle - 1].mac_cap;
  pthread_mutex_unlock(&g_cap_mu);

  /* Try real hardware capture first when active */
  if (mc) {
    int32_t got = fv_av_macos_capture_read_frame(mc, out_buf, cap);
    if (got > 0) {
      return got;
    }
  }

  /* Explicit fallback frame (reason recorded in slot->reason, never masquerades as real HW) */
  if (kind == FV_CAPTURE_AUDIO) {
    /* 20ms @ 48kHz mono s16le = 960 samples = 1920 bytes, 440Hz sine wave */
    int32_t nbytes = 1920;
    if (nbytes > cap) nbytes = cap & ~1;
    int32_t nsamples = nbytes / 2;
    double phase0 = (double)(seq * 960u) * (2.0 * 3.14159265358979323846 * 440.0 / 48000.0);
    double step = 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
    for (int32_t i = 0; i < nsamples; i++) {
      int16_t s = (int16_t)(sin(phase0 + (double)i * step) * 12000.0);
      out_buf[i * 2] = (uint8_t)(s & 0xFF);
      out_buf[i * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
    return nbytes;
  }

  /* Video or Screen fallback: RGBA deterministic test pattern */
  int64_t total64 = (int64_t)w * (int64_t)h * 4;
  int32_t nbytes = (total64 > cap) ? (cap & ~3) : (int32_t)total64;
  uint8_t base_r = (kind == FV_CAPTURE_SCREEN) ? 32 : 64;
  uint8_t base_g = (kind == FV_CAPTURE_SCREEN) ? 96 : 128;
  uint8_t base_b = (uint8_t)((seq * 17u) & 0xFFu);
  int32_t npix = nbytes / 4;
  for (int32_t i = 0; i < npix; i++) {
    out_buf[i * 4 + 0] = (uint8_t)(base_r + ((i + (int32_t)seq) & 0x3F));
    out_buf[i * 4 + 1] = base_g;
    out_buf[i * 4 + 2] = base_b;
    out_buf[i * 4 + 3] = 255;
  }
  return nbytes;
}

void fv_capture_close(int32_t handle) {
  if (handle < 1 || handle > MAX_CAPTURE_SLOTS) return;
  FvMacCapture *mc = NULL;
  pthread_mutex_lock(&g_cap_mu);
  if (g_cap_slots[handle - 1].used) {
    mc = g_cap_slots[handle - 1].mac_cap;
    memset(&g_cap_slots[handle - 1], 0, sizeof(CaptureSlot));
  }
  pthread_mutex_unlock(&g_cap_mu);
  if (mc) {
    fv_av_macos_capture_close(mc);
  }
}

int32_t fv_capture_active_count(void) {
  pthread_mutex_lock(&g_cap_mu);
  int32_t cnt = 0;
  for (int i = 0; i < MAX_CAPTURE_SLOTS; i++) {
    if (g_cap_slots[i].used) cnt++;
  }
  pthread_mutex_unlock(&g_cap_mu);
  return cnt;
}

int32_t fv_webrtc_audio_out_open(int32_t sample_rate, int32_t channels, int32_t flags) {
  return fv_audio_out_open(sample_rate, channels, FV_AUDIO_FMT_S16, flags);
}

int32_t fv_webrtc_audio_out_write(int32_t handle, const uint8_t *pcm, int32_t len) {
  return fv_audio_out_write(handle, pcm, len);
}

void fv_webrtc_audio_out_close(int32_t handle) {
  fv_audio_out_close(handle);
}

int32_t fv_webrtc_random_bytes(uint8_t *out_buf, int32_t len) {
  if (!out_buf || len <= 0 || len > 65536) return -1;
  return RAND_bytes(out_buf, (size_t)len) == 1 ? len : -1;
}
