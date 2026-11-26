#include "c_compat/moonbit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../../../native/win_compat.h"
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

extern moonbit_bytes_t fv_wa_sha256(moonbit_bytes_t data);
extern moonbit_bytes_t fv_wa_random(int32_t n);
extern moonbit_bytes_t fv_wa_keygen(int32_t alg);
extern int32_t fv_wa_alg(moonbit_bytes_t pkcs8);
extern moonbit_bytes_t fv_wa_sign(moonbit_bytes_t pkcs8, moonbit_bytes_t data);
extern int32_t fv_wa_save(moonbit_bytes_t path, moonbit_bytes_t data);

// BoringSSL verification symbols from libcurl-impersonate.a
typedef struct env_md_st EVP_MD;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct env_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;
extern const EVP_MD *EVP_sha256(void);
extern EVP_PKEY *d2i_PUBKEY(EVP_PKEY **, const uint8_t **, long);
extern int EVP_PKEY_id(const EVP_PKEY *);
extern void EVP_PKEY_free(EVP_PKEY *);
extern EVP_MD_CTX *EVP_MD_CTX_new(void);
extern void EVP_MD_CTX_free(EVP_MD_CTX *);
extern int EVP_DigestVerifyInit(EVP_MD_CTX *, EVP_PKEY_CTX **, const EVP_MD *, ENGINE *, EVP_PKEY *);
extern int EVP_DigestVerify(EVP_MD_CTX *, const uint8_t *, size_t, const uint8_t *, size_t);

enum { kEd25519Id = 949 };

static moonbit_bytes_t wrap_bytes(const void *p, int32_t len) {
  moonbit_bytes_t b = moonbit_make_bytes(len, 0);
  if (len > 0 && p) memcpy(b, p, (size_t)len);
  return b;
}

int32_t pion_wa_mb_len(void *b) {
  return fv_mb_bytes_len((moonbit_bytes_t)b);
}

void pion_wa_mb_free(void *b) {
  fv_mb_bytes_free((moonbit_bytes_t)b);
}

void *pion_wa_sha256(const void *data, int32_t len) {
  moonbit_bytes_t mb = wrap_bytes(data, len);
  moonbit_bytes_t out = fv_wa_sha256(mb);
  fv_mb_bytes_free(mb);
  return (void *)out;
}

void *pion_wa_random(int32_t n) {
  return (void *)fv_wa_random(n);
}

void *pion_wa_keygen(int32_t alg) {
  return (void *)fv_wa_keygen(alg);
}

int32_t pion_wa_alg(const void *pkcs8, int32_t len) {
  moonbit_bytes_t mb = wrap_bytes(pkcs8, len);
  int32_t a = fv_wa_alg(mb);
  fv_mb_bytes_free(mb);
  return a;
}

void *pion_wa_sign(const void *pkcs8, int32_t plen, const void *data, int32_t dlen) {
  moonbit_bytes_t mp = wrap_bytes(pkcs8, plen);
  moonbit_bytes_t md = wrap_bytes(data, dlen);
  moonbit_bytes_t sig = fv_wa_sign(mp, md);
  fv_mb_bytes_free(mp);
  fv_mb_bytes_free(md);
  return (void *)sig;
}

int32_t pion_wa_save(const void *path, int32_t plen, const void *data, int32_t dlen) {
  moonbit_bytes_t mp = wrap_bytes(path, plen);
  moonbit_bytes_t md = wrap_bytes(data, dlen);
  int32_t ok = fv_wa_save(mp, md);
  fv_mb_bytes_free(mp);
  fv_mb_bytes_free(md);
  return ok;
}

void *pion_wa_read_file(const void *path_cstr) {
  FILE *f = fopen((const char *)path_cstr, "rb");
  if (!f) return (void *)moonbit_make_bytes(0, 0);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return (void *)moonbit_make_bytes(0, 0);
  }
  rewind(f);
  moonbit_bytes_t out = moonbit_make_bytes((int32_t)sz, 0);
  size_t got = fread(out, 1, (size_t)sz, f);
  fclose(f);
  ((fv_mb_bytes_hdr_t *)(out - offsetof(fv_mb_bytes_hdr_t, data)))->len = (int64_t)got;
  return (void *)out;
}

int32_t pion_wa_verify(
    const void *spki, int32_t slen,
    const void *data, int32_t dlen,
    const void *sig, int32_t siglen) {
  const uint8_t *p = (const uint8_t *)spki;
  EVP_PKEY *k = d2i_PUBKEY(NULL, &p, (long)slen);
  if (!k) return 0;
  int id = EVP_PKEY_id(k);
  const EVP_MD *md = (id == kEd25519Id) ? NULL : EVP_sha256();
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  int ok = 0;
  if (ctx && EVP_DigestVerifyInit(ctx, NULL, md, NULL, k) == 1) {
    ok = EVP_DigestVerify(ctx, (const uint8_t *)sig, (size_t)siglen, (const uint8_t *)data, (size_t)dlen) == 1 ? 1 : 0;
  }
  if (ctx) EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(k);
  return ok;
}

void pion_wa_mkdir(const void *dir_cstr) {
  if (dir_cstr) mkdir((const char *)dir_cstr, 0700);
}

void pion_wa_unlink(const void *path_cstr) {
  if (path_cstr) unlink((const char *)path_cstr);
}
