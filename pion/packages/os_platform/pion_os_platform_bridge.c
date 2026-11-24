#include "../../../native/page/os_platform.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const void *p, int32_t len) {
  if (!p || len < 0) return NULL;
  char *s = (char *)malloc((size_t)len + 1u);
  if (!s) return NULL;
  if (len > 0) memcpy(s, p, (size_t)len);
  s[len] = '\0';
  return s;
}

void pion_os_clip_set_mem_mode(int32_t enabled) {
  fv_clipboard_set_mem_mode(enabled);
}

int32_t pion_os_clip_is_mem_mode(void) {
  return fv_clipboard_is_mem_mode();
}

int32_t pion_os_clip_probe_os(void) {
  return fv_clipboard_probe_os();
}

int32_t pion_os_clip_clear(void) {
  return fv_clipboard_clear();
}

int32_t pion_os_clip_write_item(const void *mime, int32_t mime_len,
                                const void *data, int32_t data_len,
                                int32_t clear_first) {
  char *m = dup_cstr(mime, mime_len);
  if (!m) return 0;
  int32_t r = fv_clipboard_write_item(m, data, data_len, clear_first);
  free(m);
  return r;
}

int32_t pion_os_clip_read(const void *mime, int32_t mime_len,
                          void *out_buf, int32_t cap) {
  char *m = dup_cstr(mime, mime_len);
  if (!m) return -1;
  int32_t r = fv_clipboard_read(m, out_buf, cap);
  free(m);
  return r;
}

int32_t pion_os_clip_types(void *out_buf, int32_t cap) {
  return fv_clipboard_types((char *)out_buf, cap);
}

int32_t pion_os_proxy_host_matches_bypass(const void *host, int32_t host_len,
                                          const void *rule, int32_t rule_len) {
  char *h = dup_cstr(host, host_len);
  char *r = dup_cstr(rule, rule_len);
  int32_t res = 0;
  if (h && r) res = fv_proxy_host_matches_bypass(h, r);
  free(h);
  free(r);
  return res;
}

int32_t pion_os_proxy_url_is_bypassed(const void *url, int32_t url_len,
                                      const void *list, int32_t list_len) {
  char *u = dup_cstr(url, url_len);
  char *l = dup_cstr(list, list_len);
  int32_t res = 0;
  if (u && l) res = fv_proxy_url_is_bypassed(u, l);
  free(u);
  free(l);
  return res;
}

int32_t pion_os_system_proxy_for_url(const void *url, int32_t url_len,
                                     void *out_buf, int32_t cap) {
  char *u = dup_cstr(url, url_len);
  if (!u) return -1;
  int32_t res = fv_system_proxy_for_url(u, (char *)out_buf, cap);
  free(u);
  return res;
}

int32_t pion_os_vault_seal(const void *key32,
                           const void *pt, int32_t pt_len,
                           const void *aad, int32_t aad_len,
                           void *out_buf, int32_t out_cap) {
  return fv_vault_seal((const uint8_t *)key32,
                       (const uint8_t *)pt, pt_len,
                       (const uint8_t *)aad, aad_len,
                       (uint8_t *)out_buf, out_cap);
}

int32_t pion_os_vault_open(const void *key32,
                           const void *sealed, int32_t sealed_len,
                           const void *aad, int32_t aad_len,
                           void *out_buf, int32_t out_cap) {
  return fv_vault_open((const uint8_t *)key32,
                       (const uint8_t *)sealed, sealed_len,
                       (const uint8_t *)aad, aad_len,
                       (uint8_t *)out_buf, out_cap);
}

int32_t pion_os_keychain_probe_os(void) {
  return fv_keychain_probe_os();
}

int32_t pion_os_vault_get_or_create_master_key(const void *svc, int32_t svc_len,
                                               const void *acc, int32_t acc_len,
                                               void *out_key32) {
  char *s = dup_cstr(svc, svc_len);
  char *a = dup_cstr(acc, acc_len);
  int32_t res = 0;
  if (s && a && out_key32) {
    res = fv_vault_get_or_create_master_key(s, a, (uint8_t *)out_key32);
  }
  free(s);
  free(a);
  return res;
}

int32_t pion_os_setenv(const void *name, int32_t name_len,
                       const void *val, int32_t val_len) {
  char *n = dup_cstr(name, name_len);
  char *v = dup_cstr(val, val_len);
  int32_t res = 0;
  if (n && v) res = fv_os_setenv(n, v);
  free(n);
  free(v);
  return res;
}

int32_t pion_os_unsetenv(const void *name, int32_t name_len) {
  char *n = dup_cstr(name, name_len);
  int32_t res = 0;
  if (n) res = fv_os_unsetenv(n);
  free(n);
  return res;
}
