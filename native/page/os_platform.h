#ifndef FARVIEW_OS_PLATFORM_H
#define FARVIEW_OS_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. OS-level Rich Clipboard (macOS NSPasteboard via dlopen AppKit + ObjC,
 *    with FV_CLIPBOARD_MEM=1 / Linux in-process multi-format isolation).
 * ============================================================================ */

/* Enable or disable in-process memory-isolated clipboard mode.
 * When enabled (or when FV_CLIPBOARD_MEM=1, or on non-macOS), clipboard calls
 * never touch the host OS pasteboard. */
void fv_clipboard_set_mem_mode(int32_t enabled);
int32_t fv_clipboard_is_mem_mode(void);

/* Read-only probe: returns 1 if macOS NSPasteboard was dynamically loaded via
 * dlopen(AppKit) + ObjC runtime and generalPasteboard is reachable; 0 otherwise. */
int32_t fv_clipboard_probe_os(void);

/* Clear all formats from the clipboard. Returns 1 on success, 0 on failure. */
int32_t fv_clipboard_clear(void);

/* Write a single MIME format item.
 * Supported standard MIME types:
 *   "text/plain"    -> public.utf8-plain-text
 *   "text/html"     -> public.html
 *   "image/png"     -> public.png
 *   "text/uri-list" -> public.file-url
 * If clear_first != 0, existing clipboard contents are cleared first;
 * if clear_first == 0, the format is added alongside existing formats.
 * Returns 1 on success, 0 on failure. */
int32_t fv_clipboard_write_item(const char *mime, const void *data, int32_t len, int32_t clear_first);

/* Read a MIME format from the clipboard.
 * If out_buf == NULL or cap <= 0, returns the byte length if present, or -1 if absent.
 * Otherwise copies up to cap bytes into out_buf and returns bytes copied (or -1 if absent). */
int32_t fv_clipboard_read(const char *mime, void *out_buf, int32_t cap);

/* Enumerate available MIME types as a '\n'-separated UTF-8 string.
 * Returns bytes written (excluding NUL), or 0 if empty. */
int32_t fv_clipboard_types(char *out_buf, int32_t cap);

/* ============================================================================
 * 2. System Proxy Auto-Detection & Bypass Rules
 *    (Env HTTPS_PROXY/HTTP_PROXY/ALL_PROXY/NO_PROXY + macOS CFNetwork dlopen).
 * ============================================================================ */

/* Returns 1 if `host` matches a single bypass rule (exact, localhost, *.local,
 * .suffix, IPv4/IPv6 CIDR including macOS short-form e.g. 169.254/16, or *). */
int32_t fv_proxy_host_matches_bypass(const char *host, const char *rule);

/* Returns 1 if the URL's host matches any comma-separated rule in `no_proxy_list`. */
int32_t fv_proxy_url_is_bypassed(const char *url, const char *no_proxy_list);

/* Resolve proxy URI for `url` into `out_proxy_buf` (e.g. "http://127.0.0.1:7890"
 * or "socks5h://127.0.0.1:1080"). Writes "" and returns 0 for direct connection.
 * Returns length written (> 0) when a proxy applies, 0 for direct, < 0 on error. */
int32_t fv_system_proxy_for_url(const char *url, char *out_proxy_buf, int32_t cap);

/* ============================================================================
 * 3. Credential / Cookie Vault Encryption (AES-256-GCM via BoringSSL +
 *    macOS Keychain via dlopen Security.framework).
 * ============================================================================ */

/* AES-256-GCM seal: out = nonce(12B) || ciphertext(pt_len) || tag(16B).
 * Returns total bytes written (12 + pt_len + 16), or -1 on error. */
int32_t fv_vault_seal(const uint8_t *key32,
                      const uint8_t *plaintext, int32_t pt_len,
                      const uint8_t *aad, int32_t aad_len,
                      uint8_t *out_buf, int32_t out_cap);

/* AES-256-GCM open: verifies 16B tag and decrypts.
 * Returns plaintext length (>= 0) on success, or -1 if tampered / wrong key / invalid. */
int32_t fv_vault_open(const uint8_t *key32,
                      const uint8_t *sealed, int32_t sealed_len,
                      const uint8_t *aad, int32_t aad_len,
                      uint8_t *out_buf, int32_t out_cap);

/* Read-only probe: returns 1 if macOS Security.framework Keychain symbols were
 * dynamically loaded via dlopen; 0 otherwise. */
int32_t fv_keychain_probe_os(void);

/* Keychain generic password helpers (macOS dlopen Security.framework).
 * Never invoked when FV_VAULT_TEST_KEY is set. */
int32_t fv_keychain_write(const char *service, const char *account,
                          const uint8_t *data, int32_t len);
int32_t fv_keychain_read(const char *service, const char *account,
                         uint8_t *out_buf, int32_t cap);
int32_t fv_keychain_delete(const char *service, const char *account);

/* Obtain or create a 32-byte master key.
 * If env FV_VAULT_TEST_KEY is set and non-empty, derives a deterministic 32-byte
 * key without touching macOS Keychain (for headless CI tests).
 * Returns 1 on success, 0 on failure. */
int32_t fv_vault_get_or_create_master_key(const char *service, const char *account,
                                          uint8_t out_key32[32]);

/* Process environment helpers for deterministic testing. */
int32_t fv_os_setenv(const char *name, const char *val);
int32_t fv_os_unsetenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FARVIEW_OS_PLATFORM_H */
