// The platform authenticator's keys (webauthn.mbt): a passkey made (P-256, or Ed25519 where a site takes only that),
// signed with, and the profile's passkeys file written whole in its place. The BoringSSL of curl-impersonate's library
// the host links does it (src/js/crypto.cc's way: its headers are not here, the archive is, so only through pointers).
#include <moonbit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct env_md_st EVP_MD;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct env_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;
typedef struct pkcs8_priv_key_info_st PKCS8_PRIV_KEY_INFO;
const EVP_MD* EVP_sha256(void);
EVP_PKEY_CTX* EVP_PKEY_CTX_new_id(int, ENGINE*);
void EVP_PKEY_CTX_free(EVP_PKEY_CTX*);
int EVP_PKEY_keygen_init(EVP_PKEY_CTX*);
int EVP_PKEY_keygen(EVP_PKEY_CTX*, EVP_PKEY**);
int EVP_PKEY_CTX_set_ec_paramgen_curve_nid(EVP_PKEY_CTX*, int);
void EVP_PKEY_free(EVP_PKEY*);
int EVP_PKEY_id(const EVP_PKEY*);
int i2d_PUBKEY(const EVP_PKEY*, uint8_t**);
EVP_PKEY* d2i_AutoPrivateKey(EVP_PKEY**, const uint8_t**, long);
PKCS8_PRIV_KEY_INFO* EVP_PKEY2PKCS8(const EVP_PKEY*);
int i2d_PKCS8_PRIV_KEY_INFO(PKCS8_PRIV_KEY_INFO*, uint8_t**);
void PKCS8_PRIV_KEY_INFO_free(PKCS8_PRIV_KEY_INFO*);
EVP_MD_CTX* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX*);
int EVP_DigestSignInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int EVP_DigestSign(EVP_MD_CTX*, uint8_t*, size_t*, const uint8_t*, size_t);
uint8_t* SHA256(const uint8_t*, size_t, uint8_t*);
int RAND_bytes(uint8_t*, size_t);
void OPENSSL_free(void*);

enum { kEC = 408, kEd25519 = 949, kP256 = 415 };

static moonbit_bytes_t empty(void) { return moonbit_make_bytes(0, 0); }

static moonbit_bytes_t copied(const uint8_t* p, size_t n) {
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)n, 0);
  if (n) memcpy(out, p, n);
  return out;
}

// SHA-256 of `data`.
moonbit_bytes_t fv_wa_sha256(moonbit_bytes_t data) {
  uint8_t out[32];
  SHA256((const uint8_t*)data, (size_t)Moonbit_array_length(data), out);
  return copied(out, 32);
}

// `n` bytes from the system's random source (empty where it gives none).
moonbit_bytes_t fv_wa_random(int32_t n) {
  if (n <= 0) return empty();
  moonbit_bytes_t out = moonbit_make_bytes(n, 0);
  if (RAND_bytes((uint8_t*)out, (size_t)n) != 1) return empty();
  return out;
}

// A new key: -7 (ES256, P-256) or -8 (EdDSA, Ed25519). Its PKCS#8 and its SubjectPublicKeyInfo, as
// [PKCS#8's length, 2 bytes big-endian][PKCS#8][SPKI]; empty where none could be made.
moonbit_bytes_t fv_wa_keygen(int32_t alg) {
  int id = alg == -8 ? kEd25519 : kEC;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(id, NULL);
  EVP_PKEY* k = NULL;
  int ok = ctx && EVP_PKEY_keygen_init(ctx) == 1;
  if (ok && id == kEC) ok = EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, kP256) == 1;
  ok = ok && EVP_PKEY_keygen(ctx, &k) == 1 && k;
  if (ctx) EVP_PKEY_CTX_free(ctx);
  if (!ok) return empty();
  uint8_t *p8 = NULL, *spki = NULL;
  int n8 = -1, ns = -1;
  PKCS8_PRIV_KEY_INFO* info = EVP_PKEY2PKCS8(k);
  if (info) {
    n8 = i2d_PKCS8_PRIV_KEY_INFO(info, &p8);
    PKCS8_PRIV_KEY_INFO_free(info);
  }
  ns = i2d_PUBKEY(k, &spki);
  EVP_PKEY_free(k);
  moonbit_bytes_t out = empty();
  if (n8 > 0 && n8 < 65536 && ns > 0) {
    out = moonbit_make_bytes(2 + n8 + ns, 0);
    uint8_t* o = (uint8_t*)out;
    o[0] = (uint8_t)(n8 >> 8);
    o[1] = (uint8_t)n8;
    memcpy(o + 2, p8, (size_t)n8);
    memcpy(o + 2 + n8, spki, (size_t)ns);
  }
  if (p8) OPENSSL_free(p8);
  if (spki) OPENSSL_free(spki);
  return out;
}

// What a PKCS#8 key is: -7 (P-256), -8 (Ed25519), 0 (none this authenticator signs with).
int32_t fv_wa_alg(moonbit_bytes_t pkcs8) {
  const uint8_t* p = (const uint8_t*)pkcs8;
  EVP_PKEY* k = d2i_AutoPrivateKey(NULL, &p, (long)Moonbit_array_length(pkcs8));
  if (!k) return 0;
  int id = EVP_PKEY_id(k);
  EVP_PKEY_free(k);
  return id == kEC ? -7 : id == kEd25519 ? -8 : 0;
}

// `data` signed with the PKCS#8 key: an ECDSA signature in ASN.1 DER over its SHA-256 (as WebAuthn's ES256 has it),
// or Ed25519's 64 bytes; empty where it could not be.
moonbit_bytes_t fv_wa_sign(moonbit_bytes_t pkcs8, moonbit_bytes_t data) {
  const uint8_t* p = (const uint8_t*)pkcs8;
  EVP_PKEY* k = d2i_AutoPrivateKey(NULL, &p, (long)Moonbit_array_length(pkcs8));
  if (!k) return empty();
  int id = EVP_PKEY_id(k);
  const EVP_MD* md = id == kEd25519 ? NULL : EVP_sha256();
  EVP_MD_CTX* m = EVP_MD_CTX_new();
  size_t n = 0;
  moonbit_bytes_t out = empty();
  if ((id == kEC || id == kEd25519) && EVP_DigestSignInit(m, NULL, md, NULL, k) == 1 &&
      EVP_DigestSign(m, NULL, &n, (const uint8_t*)data, (size_t)Moonbit_array_length(data)) == 1) {
    uint8_t* sig = (uint8_t*)malloc(n + 1);
    if (sig && EVP_DigestSign(m, sig, &n, (const uint8_t*)data, (size_t)Moonbit_array_length(data)) == 1) out = copied(sig, n);
    free(sig);
  }
  EVP_MD_CTX_free(m);
  EVP_PKEY_free(k);
  return out;
}

// `data` written to `path` (UTF-8, no NUL) whole or not at all: a file beside it, readable by its owner alone, put in
// its place. 1 when it was.
int32_t fv_wa_save(moonbit_bytes_t path, moonbit_bytes_t data) {
  size_t pn = (size_t)Moonbit_array_length(path);
  char* dst = (char*)malloc(pn + 1);
  char* tmp = (char*)malloc(pn + 32);
  if (!dst || !tmp) {
    free(dst);
    free(tmp);
    return 0;
  }
  memcpy(dst, path, pn);
  dst[pn] = 0;
  snprintf(tmp, pn + 32, "%s.%d.tmp", dst, (int)getpid());
  int ok = 0;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    const uint8_t* b = (const uint8_t*)data;
    size_t left = (size_t)Moonbit_array_length(data);
    ok = 1;
    while (left > 0) {
      ssize_t w = write(fd, b, left);
      if (w <= 0) {
        ok = 0;
        break;
      }
      b += w;
      left -= (size_t)w;
    }
    if (ok) ok = fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(tmp, dst) == 0;
    if (!ok) unlink(tmp);
  }
  free(dst);
  free(tmp);
  return ok;
}
