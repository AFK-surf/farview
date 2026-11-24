// The Web Crypto API's primitives, from the BoringSSL the host links (with curl): one function scripts call as
// __fvCrypto(op, ...), the algorithm checks and key formats done in the prelude. A failure returns undefined (the
// prelude's OperationError).
#ifdef __APPLE__
// What the BoringSSL of curl-impersonate's library needs on macOS, said in this object (src/fetch/fetch.c's way)
__asm__(".linker_option \"-framework\", \"Security\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n");
#endif
#include <v8.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace v8;

// BoringSSL's types and functions (its headers are not here; the archive is): only through pointers
extern "C" {
typedef struct env_md_st EVP_MD;
typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct env_md_ctx_st EVP_MD_CTX;
typedef struct engine_st ENGINE;
typedef struct bignum_st BIGNUM;
typedef struct ecdsa_sig_st ECDSA_SIG;
typedef struct ec_key_st EC_KEY;
typedef struct ec_group_st EC_GROUP;
typedef struct pkcs8_priv_key_info_st PKCS8_PRIV_KEY_INFO;
const EVP_MD* EVP_sha1(void);
const EVP_MD* EVP_sha256(void);
const EVP_MD* EVP_sha384(void);
const EVP_MD* EVP_sha512(void);
uint8_t* HMAC(const EVP_MD*, const void*, size_t, const uint8_t*, size_t, uint8_t*, unsigned int*);
int PKCS5_PBKDF2_HMAC(const char*, size_t, const uint8_t*, size_t, uint32_t, const EVP_MD*, size_t, uint8_t*);
int HKDF(uint8_t*, size_t, const EVP_MD*, const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t);
const EVP_CIPHER* EVP_aes_128_cbc(void);
const EVP_CIPHER* EVP_aes_192_cbc(void);
const EVP_CIPHER* EVP_aes_256_cbc(void);
const EVP_CIPHER* EVP_aes_128_ctr(void);
const EVP_CIPHER* EVP_aes_192_ctr(void);
const EVP_CIPHER* EVP_aes_256_ctr(void);
const EVP_CIPHER* EVP_aes_128_gcm(void);
const EVP_CIPHER* EVP_aes_192_gcm(void);
const EVP_CIPHER* EVP_aes_256_gcm(void);
EVP_CIPHER_CTX* EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX*);
int EVP_CipherInit_ex(EVP_CIPHER_CTX*, const EVP_CIPHER*, ENGINE*, const uint8_t*, const uint8_t*, int);
int EVP_CipherUpdate(EVP_CIPHER_CTX*, uint8_t*, int*, const uint8_t*, int);
int EVP_CipherFinal_ex(EVP_CIPHER_CTX*, uint8_t*, int*);
int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX*, int, int, void*);
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX*, int);
int AES_set_encrypt_key(const uint8_t*, unsigned, void*);
int AES_set_decrypt_key(const uint8_t*, unsigned, void*);
int AES_wrap_key(const void*, const uint8_t*, uint8_t*, const uint8_t*, size_t);
int AES_unwrap_key(const void*, const uint8_t*, uint8_t*, const uint8_t*, size_t);
EVP_PKEY_CTX* EVP_PKEY_CTX_new_id(int, ENGINE*);
EVP_PKEY_CTX* EVP_PKEY_CTX_new(EVP_PKEY*, ENGINE*);
void EVP_PKEY_CTX_free(EVP_PKEY_CTX*);
int EVP_PKEY_keygen_init(EVP_PKEY_CTX*);
int EVP_PKEY_keygen(EVP_PKEY_CTX*, EVP_PKEY**);
int EVP_PKEY_CTX_set_rsa_keygen_bits(EVP_PKEY_CTX*, int);
int EVP_PKEY_CTX_set_rsa_keygen_pubexp(EVP_PKEY_CTX*, BIGNUM*);
int EVP_PKEY_CTX_set_ec_paramgen_curve_nid(EVP_PKEY_CTX*, int);
void EVP_PKEY_free(EVP_PKEY*);
int i2d_PUBKEY(const EVP_PKEY*, uint8_t**);
EVP_PKEY* d2i_PUBKEY(EVP_PKEY**, const uint8_t**, long);
EVP_PKEY* d2i_AutoPrivateKey(EVP_PKEY**, const uint8_t**, long);
PKCS8_PRIV_KEY_INFO* EVP_PKEY2PKCS8(const EVP_PKEY*);
int i2d_PKCS8_PRIV_KEY_INFO(PKCS8_PRIV_KEY_INFO*, uint8_t**);
void PKCS8_PRIV_KEY_INFO_free(PKCS8_PRIV_KEY_INFO*);
EVP_MD_CTX* EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX*);
int EVP_DigestSignInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int EVP_DigestSign(EVP_MD_CTX*, uint8_t*, size_t*, const uint8_t*, size_t);
int EVP_DigestVerifyInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int EVP_DigestVerify(EVP_MD_CTX*, const uint8_t*, size_t, const uint8_t*, size_t);
int EVP_PKEY_CTX_set_rsa_padding(EVP_PKEY_CTX*, int);
int EVP_PKEY_CTX_set_rsa_pss_saltlen(EVP_PKEY_CTX*, int);
int EVP_PKEY_CTX_set_rsa_mgf1_md(EVP_PKEY_CTX*, const EVP_MD*);
int EVP_PKEY_CTX_set_rsa_oaep_md(EVP_PKEY_CTX*, const EVP_MD*);
int EVP_PKEY_CTX_set0_rsa_oaep_label(EVP_PKEY_CTX*, uint8_t*, size_t);
int EVP_PKEY_encrypt_init(EVP_PKEY_CTX*);
int EVP_PKEY_encrypt(EVP_PKEY_CTX*, uint8_t*, size_t*, const uint8_t*, size_t);
int EVP_PKEY_decrypt_init(EVP_PKEY_CTX*);
int EVP_PKEY_decrypt(EVP_PKEY_CTX*, uint8_t*, size_t*, const uint8_t*, size_t);
int EVP_PKEY_derive_init(EVP_PKEY_CTX*);
int EVP_PKEY_derive_set_peer(EVP_PKEY_CTX*, EVP_PKEY*);
int EVP_PKEY_derive(EVP_PKEY_CTX*, uint8_t*, size_t*);
int EVP_PKEY_id(const EVP_PKEY*);
int EVP_PKEY_bits(const EVP_PKEY*);
EC_KEY* EVP_PKEY_get0_EC_KEY(const EVP_PKEY*);
const EC_GROUP* EC_KEY_get0_group(const EC_KEY*);
int EC_GROUP_get_curve_name(const EC_GROUP*);
int EC_KEY_check_key(const EC_KEY*);
ECDSA_SIG* ECDSA_SIG_from_bytes(const uint8_t*, size_t);
int ECDSA_SIG_to_bytes(uint8_t**, size_t*, const ECDSA_SIG*);
ECDSA_SIG* ECDSA_SIG_new(void);
void ECDSA_SIG_free(ECDSA_SIG*);
void ECDSA_SIG_get0(const ECDSA_SIG*, const BIGNUM**, const BIGNUM**);
int ECDSA_SIG_set0(ECDSA_SIG*, BIGNUM*, BIGNUM*);
BIGNUM* BN_bin2bn(const uint8_t*, size_t, BIGNUM*);
int BN_bn2bin_padded(uint8_t*, size_t, const BIGNUM*);
void BN_free(BIGNUM*);
void* OPENSSL_malloc(size_t);
int RAND_bytes(uint8_t*, size_t);
void OPENSSL_free(void*);
}

namespace {
const int kRSA = 6, kEC = 408, kX25519 = 948, kEd25519 = 949;
const int kP256 = 415, kP384 = 715, kP521 = 716;

struct Bytes {
  const uint8_t* p = nullptr;
  size_t n = 0;
  std::shared_ptr<BackingStore> keep;
  bool ok = false;
};

Bytes bytes(Local<Value> v) {
  Bytes b;
  if (v->IsArrayBufferView()) {
    Local<ArrayBufferView> view = v.As<ArrayBufferView>();
    b.keep = view->Buffer()->GetBackingStore();
    b.p = static_cast<const uint8_t*>(b.keep->Data()) + view->ByteOffset();
    b.n = view->ByteLength();
    b.ok = true;
  } else if (v->IsArrayBuffer()) {
    b.keep = v.As<ArrayBuffer>()->GetBackingStore();
    b.p = static_cast<const uint8_t*>(b.keep->Data());
    b.n = b.keep->ByteLength();
    b.ok = true;
  }
  if (b.ok && !b.p) b.p = reinterpret_cast<const uint8_t*>("");
  return b;
}

Local<ArrayBuffer> buffer(Isolate* i, const uint8_t* p, size_t n) {
  Local<ArrayBuffer> ab = ArrayBuffer::New(i, n);
  if (n) memcpy(ab->GetBackingStore()->Data(), p, n);
  return ab;
}

std::string str(Isolate* i, Local<Value> v) {
  String::Utf8Value u(i, v);
  return std::string(*u ? *u : "");
}

const EVP_MD* md(const std::string& n) {
  if (n == "SHA-1") return EVP_sha1();
  if (n == "SHA-256") return EVP_sha256();
  if (n == "SHA-384") return EVP_sha384();
  if (n == "SHA-512") return EVP_sha512();
  return nullptr;
}

int curve_nid(const std::string& c) { return c == "P-256" ? kP256 : c == "P-384" ? kP384 : c == "P-521" ? kP521 : 0; }
const char* curve_name(int nid) { return nid == kP256 ? "P-256" : nid == kP384 ? "P-384" : nid == kP521 ? "P-521" : ""; }
size_t curve_bytes(int nid) { return nid == kP256 ? 32 : nid == kP384 ? 48 : nid == kP521 ? 66 : 0; }

EVP_PKEY* parse(const Bytes& b, bool priv) {
  const uint8_t* p = b.p;
  return priv ? d2i_AutoPrivateKey(nullptr, &p, (long)b.n) : d2i_PUBKEY(nullptr, &p, (long)b.n);
}

Local<Value> pubkey_der(Isolate* i, EVP_PKEY* k) {
  uint8_t* out = nullptr;
  int n = i2d_PUBKEY(k, &out);
  if (n <= 0) return Undefined(i);
  Local<ArrayBuffer> ab = buffer(i, out, (size_t)n);
  OPENSSL_free(out);
  return ab;
}

Local<Value> privkey_der(Isolate* i, EVP_PKEY* k) {
  PKCS8_PRIV_KEY_INFO* p8 = EVP_PKEY2PKCS8(k);
  if (!p8) return Undefined(i);
  uint8_t* out = nullptr;
  int n = i2d_PKCS8_PRIV_KEY_INFO(p8, &out);
  PKCS8_PRIV_KEY_INFO_free(p8);
  if (n <= 0) return Undefined(i);
  Local<ArrayBuffer> ab = buffer(i, out, (size_t)n);
  OPENSSL_free(out);
  return ab;
}

void crypto_call(const FunctionCallbackInfo<Value>& a) {
  Isolate* i = a.GetIsolate();
  Local<Context> c = i->GetCurrentContext();
  if (a.Length() < 1) return;
  std::string op = str(i, a[0]);
  auto arg = [&](int k) -> Local<Value> { return k < a.Length() ? a[k] : Local<Value>(Undefined(i)); };
  auto num = [&](int k) -> double { return arg(k)->NumberValue(c).FromMaybe(0); };
  if (op == "random") {
    // random(view): the view filled from the system's random source
    Bytes b = bytes(arg(1));
    if (!b.ok) return;
    if (b.n && RAND_bytes(const_cast<uint8_t*>(b.p), b.n) != 1) return;
    a.GetReturnValue().Set(Boolean::New(i, true));
  } else if (op == "hmac") {
    const EVP_MD* h = md(str(i, arg(1)));
    Bytes key = bytes(arg(2)), data = bytes(arg(3));
    if (!h || !key.ok || !data.ok) return;
    uint8_t out[64];
    unsigned n = 0;
    if (!HMAC(h, key.p, key.n, data.p, data.n, out, &n)) return;
    a.GetReturnValue().Set(buffer(i, out, n));
  } else if (op == "pbkdf2") {
    const EVP_MD* h = md(str(i, arg(1)));
    Bytes pw = bytes(arg(2)), salt = bytes(arg(3));
    size_t n = (size_t)num(5);
    if (!h || !pw.ok || !salt.ok) return;
    std::vector<uint8_t> out(n ? n : 1);
    if (!PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(pw.p), pw.n, salt.p, salt.n, (uint32_t)num(4), h, n, out.data())) return;
    a.GetReturnValue().Set(buffer(i, out.data(), n));
  } else if (op == "hkdf") {
    const EVP_MD* h = md(str(i, arg(1)));
    Bytes ikm = bytes(arg(2)), salt = bytes(arg(3)), info = bytes(arg(4));
    size_t n = (size_t)num(5);
    if (!h || !ikm.ok || !salt.ok || !info.ok) return;
    std::vector<uint8_t> out(n ? n : 1);
    if (n && !HKDF(out.data(), n, h, ikm.p, ikm.n, salt.p, salt.n, info.p, info.n)) return;
    a.GetReturnValue().Set(buffer(i, out.data(), n));
  } else if (op == "aes") {
    // aes(mode, encrypt, key, iv, data, aad, tagBytes)
    std::string mode = str(i, arg(1));
    bool enc = arg(2)->BooleanValue(i);
    Bytes key = bytes(arg(3)), iv = bytes(arg(4)), data = bytes(arg(5)), aad = bytes(arg(6));
    size_t tag = (size_t)num(7);
    if (!key.ok || !data.ok) return;
    if (mode == "KW") {
      uint8_t ks[512];
      if ((enc ? AES_set_encrypt_key(key.p, (unsigned)key.n * 8, ks) : AES_set_decrypt_key(key.p, (unsigned)key.n * 8, ks)) != 0) return;
      std::vector<uint8_t> out(data.n + 8);
      int n = enc ? AES_wrap_key(ks, nullptr, out.data(), data.p, data.n) : AES_unwrap_key(ks, nullptr, out.data(), data.p, data.n);
      if (n < 0) return;
      a.GetReturnValue().Set(buffer(i, out.data(), (size_t)n));
      return;
    }
    const EVP_CIPHER* cipher = nullptr;
    size_t kn = key.n;
    if (mode == "CBC") cipher = kn == 16 ? EVP_aes_128_cbc() : kn == 24 ? EVP_aes_192_cbc() : kn == 32 ? EVP_aes_256_cbc() : nullptr;
    if (mode == "CTR") cipher = kn == 16 ? EVP_aes_128_ctr() : kn == 24 ? EVP_aes_192_ctr() : kn == 32 ? EVP_aes_256_ctr() : nullptr;
    if (mode == "GCM") cipher = kn == 16 ? EVP_aes_128_gcm() : kn == 24 ? EVP_aes_192_gcm() : kn == 32 ? EVP_aes_256_gcm() : nullptr;
    if (!cipher || !iv.ok) return;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<uint8_t> out(data.n + 32);
    int n1 = 0, n2 = 0;
    bool ok = EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, enc ? 1 : 0) == 1;
    size_t in_n = data.n;
    if (ok && mode == "GCM") {
      // (decrypting: the tag is the data's last bytes)
      if (!enc && in_n < tag) ok = false;
      if (!enc) in_n -= tag;
      ok = ok && EVP_CIPHER_CTX_ctrl(ctx, 0x9, (int)iv.n, nullptr) == 1;
    }
    ok = ok && EVP_CipherInit_ex(ctx, nullptr, nullptr, key.p, iv.p, enc ? 1 : 0) == 1;
    if (ok && mode == "GCM" && !enc) ok = EVP_CIPHER_CTX_ctrl(ctx, 0x11, (int)tag, const_cast<uint8_t*>(data.p + in_n)) == 1;
    if (ok && mode == "GCM" && aad.ok && aad.n) { int x = 0; ok = EVP_CipherUpdate(ctx, nullptr, &x, aad.p, (int)aad.n) == 1; }
    ok = ok && EVP_CipherUpdate(ctx, out.data(), &n1, data.p, (int)in_n) == 1;
    ok = ok && EVP_CipherFinal_ex(ctx, out.data() + n1, &n2) == 1;
    size_t total = (size_t)(n1 + n2);
    if (ok && mode == "GCM" && enc) {
      ok = EVP_CIPHER_CTX_ctrl(ctx, 0x10, (int)tag, out.data() + total) == 1;
      total += tag;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return;
    a.GetReturnValue().Set(buffer(i, out.data(), total));
  } else if (op == "keygen") {
    // keygen(kind RSA/EC/Ed25519/X25519, bits or curve, publicExponent) -> [spki, pkcs8]
    std::string kind = str(i, arg(1));
    int id = kind == "RSA" ? kRSA : kind == "EC" ? kEC : kind == "Ed25519" ? kEd25519 : kind == "X25519" ? kX25519 : 0;
    if (!id) return;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(id, nullptr);
    EVP_PKEY* k = nullptr;
    bool ok = ctx && EVP_PKEY_keygen_init(ctx) == 1;
    if (ok && id == kRSA) {
      Bytes e = bytes(arg(3));
      ok = EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)num(2)) == 1;
      if (ok && e.ok && e.n) { BIGNUM* bn = BN_bin2bn(e.p, e.n, nullptr); ok = EVP_PKEY_CTX_set_rsa_keygen_pubexp(ctx, bn) == 1; if (!ok) BN_free(bn); }
    }
    if (ok && id == kEC) ok = EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, curve_nid(str(i, arg(2)))) == 1;
    ok = ok && EVP_PKEY_keygen(ctx, &k) == 1;
    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (!ok || !k) return;
    Local<Array> pair = Array::New(i, 2);
    pair->Set(c, 0, pubkey_der(i, k)).Check();
    pair->Set(c, 1, privkey_der(i, k)).Check();
    EVP_PKEY_free(k);
    a.GetReturnValue().Set(pair);
  } else if (op == "info") {
    // info(der, private) -> [type, curve, bits, spki, pkcs8]: what a key is, its public part, re-encoded
    Bytes der = bytes(arg(1));
    bool priv = arg(2)->BooleanValue(i);
    if (!der.ok) return;
    EVP_PKEY* k = parse(der, priv);
    if (!k) return;
    int id = EVP_PKEY_id(k);
    std::string type = id == kRSA ? "RSA" : id == kEC ? "EC" : id == kEd25519 ? "Ed25519" : id == kX25519 ? "X25519" : "";
    std::string curve;
    if (id == kEC) {
      EC_KEY* ec = EVP_PKEY_get0_EC_KEY(k);
      curve = ec ? curve_name(EC_GROUP_get_curve_name(EC_KEY_get0_group(ec))) : "";
      if (priv && ec && EC_KEY_check_key(ec) != 1) { EVP_PKEY_free(k); return; }
    }
    Local<Array> r = Array::New(i, 5);
    r->Set(c, 0, String::NewFromUtf8(i, type.c_str()).ToLocalChecked()).Check();
    r->Set(c, 1, String::NewFromUtf8(i, curve.c_str()).ToLocalChecked()).Check();
    r->Set(c, 2, Number::New(i, EVP_PKEY_bits(k))).Check();
    r->Set(c, 3, pubkey_der(i, k)).Check();
    r->Set(c, 4, priv ? privkey_der(i, k) : Local<Value>(Undefined(i))).Check();
    EVP_PKEY_free(k);
    a.GetReturnValue().Set(r);
  } else if (op == "sign" || op == "verify") {
    // sign(alg, hash, pkcs8, data, saltLength) / verify(alg, hash, spki, signature, data, saltLength)
    std::string alg = str(i, arg(1));
    const EVP_MD* h = alg == "Ed25519" ? nullptr : md(str(i, arg(2)));
    if (alg != "Ed25519" && !h) return;
    bool sign = op == "sign";
    Bytes der = bytes(arg(3));
    Bytes sig = sign ? Bytes() : bytes(arg(4));
    Bytes data = bytes(arg(sign ? 4 : 5));
    int salt = (int)num(sign ? 5 : 6);
    if (!der.ok || !data.ok || (!sign && !sig.ok)) return;
    EVP_PKEY* k = parse(der, sign);
    if (!k) return;
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pc = nullptr;
    bool ok = (sign ? EVP_DigestSignInit(m, &pc, h, nullptr, k) : EVP_DigestVerifyInit(m, &pc, h, nullptr, k)) == 1;
    if (ok && alg == "RSASSA-PKCS1-v1_5") ok = EVP_PKEY_CTX_set_rsa_padding(pc, 1) == 1;
    if (ok && alg == "RSA-PSS") ok = EVP_PKEY_CTX_set_rsa_padding(pc, 6) == 1 && EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, salt) == 1 && EVP_PKEY_CTX_set_rsa_mgf1_md(pc, h) == 1;
    int nid = 0;
    if (alg == "ECDSA") { EC_KEY* ec = EVP_PKEY_get0_EC_KEY(k); nid = ec ? EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) : 0; }
    size_t half = curve_bytes(nid);
    if (ok && sign) {
      size_t n = 0;
      ok = EVP_DigestSign(m, nullptr, &n, data.p, data.n) == 1;
      std::vector<uint8_t> out(n + 1);
      ok = ok && EVP_DigestSign(m, out.data(), &n, data.p, data.n) == 1;
      if (ok && alg == "ECDSA") {
        // (DER as WebCrypto has it: r and s, each the curve's size)
        ECDSA_SIG* s = ECDSA_SIG_from_bytes(out.data(), n);
        ok = s != nullptr;
        std::vector<uint8_t> raw(2 * half);
        if (ok) { const BIGNUM *r, *ss; ECDSA_SIG_get0(s, &r, &ss); ok = BN_bn2bin_padded(raw.data(), half, r) && BN_bn2bin_padded(raw.data() + half, half, ss); ECDSA_SIG_free(s); }
        if (ok) a.GetReturnValue().Set(buffer(i, raw.data(), raw.size()));
      } else if (ok) {
        a.GetReturnValue().Set(buffer(i, out.data(), n));
      }
    } else if (ok) {
      bool good = false;
      if (alg == "ECDSA") {
        if (half && sig.n == 2 * half) {
          ECDSA_SIG* s = ECDSA_SIG_new();
          ECDSA_SIG_set0(s, BN_bin2bn(sig.p, half, nullptr), BN_bin2bn(sig.p + half, half, nullptr));
          uint8_t* d = nullptr;
          size_t dn = 0;
          if (ECDSA_SIG_to_bytes(&d, &dn, s)) { good = EVP_DigestVerify(m, d, dn, data.p, data.n) == 1; OPENSSL_free(d); }
          ECDSA_SIG_free(s);
        }
      } else {
        good = EVP_DigestVerify(m, sig.p, sig.n, data.p, data.n) == 1;
      }
      a.GetReturnValue().Set(Boolean::New(i, good));
    }
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(k);
  } else if (op == "oaep") {
    // oaep(encrypt, der, hash, data, label)
    bool enc = arg(1)->BooleanValue(i);
    Bytes der = bytes(arg(2)), data = bytes(arg(4)), label = bytes(arg(5));
    const EVP_MD* h = md(str(i, arg(3)));
    if (!der.ok || !data.ok || !h) return;
    EVP_PKEY* k = parse(der, !enc);
    if (!k) return;
    EVP_PKEY_CTX* pc = EVP_PKEY_CTX_new(k, nullptr);
    bool ok = pc && (enc ? EVP_PKEY_encrypt_init(pc) : EVP_PKEY_decrypt_init(pc)) == 1;
    ok = ok && EVP_PKEY_CTX_set_rsa_padding(pc, 4) == 1 && EVP_PKEY_CTX_set_rsa_oaep_md(pc, h) == 1 && EVP_PKEY_CTX_set_rsa_mgf1_md(pc, h) == 1;
    if (ok && label.ok && label.n) {
      uint8_t* l = static_cast<uint8_t*>(OPENSSL_malloc(label.n));
      memcpy(l, label.p, label.n);
      ok = EVP_PKEY_CTX_set0_rsa_oaep_label(pc, l, label.n) == 1;
      if (!ok) OPENSSL_free(l);
    }
    size_t n = 0;
    ok = ok && (enc ? EVP_PKEY_encrypt(pc, nullptr, &n, data.p, data.n) : EVP_PKEY_decrypt(pc, nullptr, &n, data.p, data.n)) == 1;
    std::vector<uint8_t> out(n + 1);
    ok = ok && (enc ? EVP_PKEY_encrypt(pc, out.data(), &n, data.p, data.n) : EVP_PKEY_decrypt(pc, out.data(), &n, data.p, data.n)) == 1;
    if (ok) a.GetReturnValue().Set(buffer(i, out.data(), n));
    if (pc) EVP_PKEY_CTX_free(pc);
    EVP_PKEY_free(k);
  } else if (op == "derive") {
    // derive(pkcs8, peer spki): ECDH or X25519's shared secret
    Bytes mine = bytes(arg(1)), peer = bytes(arg(2));
    if (!mine.ok || !peer.ok) return;
    EVP_PKEY* k = parse(mine, true);
    EVP_PKEY* p = parse(peer, false);
    bool ok = k && p;
    EVP_PKEY_CTX* pc = ok ? EVP_PKEY_CTX_new(k, nullptr) : nullptr;
    ok = ok && pc && EVP_PKEY_derive_init(pc) == 1 && EVP_PKEY_derive_set_peer(pc, p) == 1;
    size_t n = 0;
    ok = ok && EVP_PKEY_derive(pc, nullptr, &n) == 1;
    std::vector<uint8_t> out(n + 1);
    ok = ok && EVP_PKEY_derive(pc, out.data(), &n) == 1;
    if (ok) a.GetReturnValue().Set(buffer(i, out.data(), n));
    if (pc) EVP_PKEY_CTX_free(pc);
    if (k) EVP_PKEY_free(k);
    if (p) EVP_PKEY_free(p);
  }
}
}  // namespace

// (registered on each global by the shim)
void fv_crypto_install(Isolate* i, Local<ObjectTemplate> global) {
  global->Set(i, "__fvCrypto", FunctionTemplate::New(i, crypto_call));
}
