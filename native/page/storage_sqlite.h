#ifndef FV_STORAGE_SQLITE_H
#define FV_STORAGE_SQLITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dynamic SQLite3 availability & lifecycle (zero link-time -lsqlite3 dependency) */
int32_t fv_storage_available(void);
int32_t fv_storage_open(const void *profile_dir, int32_t dir_len);
void    fv_storage_close(void);
int64_t fv_storage_now_unix(void);
int32_t fv_storage_is_wal(const void *profile_dir, int32_t dir_len);
int32_t fv_storage_remove_db(const void *profile_dir, int32_t dir_len);

/* Heap buffer bridge for returning variable-length blobs/strings to Pion */
int32_t     fv_storage_buf_len(const void *buf);
const void *fv_storage_buf_data(const void *buf);
void        fv_storage_buf_free(void *buf);

/* Table 1: idb_kv (partitioned IndexedDB / CacheStorage key-value store) */
void   *fv_idb_get(const void *dir, int32_t dir_len,
                   const void *part, int32_t part_len,
                   const void *origin, int32_t origin_len,
                   const void *key, int32_t key_len);
int32_t fv_idb_put(const void *dir, int32_t dir_len,
                   const void *part, int32_t part_len,
                   const void *origin, int32_t origin_len,
                   const void *key, int32_t key_len,
                   const void *val, int32_t val_len);
int32_t fv_idb_del(const void *dir, int32_t dir_len,
                   const void *part, int32_t part_len,
                   const void *origin, int32_t origin_len,
                   const void *key, int32_t key_len);
void   *fv_idb_list_keys(const void *dir, int32_t dir_len,
                         const void *part, int32_t part_len,
                         const void *origin, int32_t origin_len,
                         const void *prefix, int32_t prefix_len);
int32_t fv_idb_clear_origin(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len,
                            const void *origin, int32_t origin_len);
int64_t fv_idb_usage_bytes(const void *dir, int32_t dir_len,
                           const void *part, int32_t part_len,
                           const void *origin, int32_t origin_len);

/* Table 2: http_cache (RFC 9111 persistent HTTP disk cache with LRU eviction) */
void   *fv_http_cache_get(const void *dir, int32_t dir_len,
                          const void *part, int32_t part_len,
                          const void *url, int32_t url_len,
                          int64_t now_ts);
int32_t fv_http_cache_put(const void *dir, int32_t dir_len,
                          const void *part, int32_t part_len,
                          const void *meta, int32_t meta_len,
                          int32_t status,
                          int64_t expires_at,
                          int64_t stored_at,
                          const void *body, int32_t body_len,
                          int64_t max_bytes);
int32_t fv_http_cache_touch(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len,
                            const void *url, int32_t url_len,
                            int64_t new_expires_at,
                            int64_t now_ts,
                            const void *etag, int32_t etag_len,
                            const void *lm, int32_t lm_len);
int32_t fv_http_cache_delete(const void *dir, int32_t dir_len,
                             const void *part, int32_t part_len,
                             const void *url, int32_t url_len);
int64_t fv_http_cache_size_bytes(const void *dir, int32_t dir_len,
                                 const void *part, int32_t part_len);
int32_t fv_http_cache_count(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len);

/* Table 3: permissions (per-origin persistent site permission state machine) */
void   *fv_perm_get(const void *dir, int32_t dir_len,
                    const void *part, int32_t part_len,
                    const void *origin, int32_t origin_len,
                    const void *name, int32_t name_len);
int32_t fv_perm_set(const void *dir, int32_t dir_len,
                    const void *part, int32_t part_len,
                    const void *origin, int32_t origin_len,
                    const void *name, int32_t name_len,
                    const void *state, int32_t state_len);
int32_t fv_perm_clear(const void *dir, int32_t dir_len,
                      const void *part, int32_t part_len,
                      const void *origin, int32_t origin_len);

/* Read-only external SQLite query for browser migration (zero-mutation temp copy for locked DBs) */
void   *fv_sqlite_query_ro_tsv(const void *db_path, int32_t path_len,
                               const void *sql, int32_t sql_len,
                               int32_t max_rows);

/* Loopback HTTP cache test server (for E2E RFC 9111 tests) */
int32_t fv_storage_test_http_start(int32_t max_reqs);
int32_t fv_storage_test_http_hits(void);

#ifdef __cplusplus
}
#endif

#endif /* FV_STORAGE_SQLITE_H */
