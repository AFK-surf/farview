/* Farview Wave S1: Zero-dependency dlopen SQLite3 WAL unified storage engine.
 *
 * Cross-platform linking red line:
 *   - Zero #include <sqlite3.h>, zero -lsqlite3 compile/link dependency.
 *   - Loads libsqlite3 at runtime via dlopen/dlsym:
 *       macOS: /usr/lib/libsqlite3.dylib, /opt/homebrew/opt/sqlite/lib/libsqlite3.dylib, libsqlite3.dylib
 *       Linux: libsqlite3.so.0, libsqlite3.so
 *   - Degrades gracefully (fv_storage_available() == 0) if libsqlite3 is absent at runtime.
 */

#include "storage_sqlite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../win_compat.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* Opaque SQLite3 types & constants (no <sqlite3.h>) */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);

#define FV_SQLITE_OK             0
#define FV_SQLITE_ROW            100
#define FV_SQLITE_DONE           101
#define FV_SQLITE_OPEN_READWRITE 0x00000002
#define FV_SQLITE_OPEN_CREATE    0x00000004
#define FV_SQLITE_OPEN_FULLMUTEX 0x00010000
#define FV_SQLITE_TRANSIENT      ((sqlite3_destructor_type)-1)

#define FV_HTTP_CACHE_MAX_ENTRY_BYTES (16 * 1024 * 1024)

typedef int (*fn_open_v2)(const char *, sqlite3 **, int, const char *);
typedef int (*fn_close_v2)(sqlite3 *);
typedef int (*fn_exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
typedef int (*fn_prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (*fn_step)(sqlite3_stmt *);
typedef int (*fn_finalize)(sqlite3_stmt *);
typedef int (*fn_bind_text)(sqlite3_stmt *, int, const char *, int, sqlite3_destructor_type);
typedef int (*fn_bind_blob)(sqlite3_stmt *, int, const void *, int, sqlite3_destructor_type);
typedef int (*fn_bind_int)(sqlite3_stmt *, int, int);
typedef int (*fn_bind_int64)(sqlite3_stmt *, int, int64_t);
typedef const void *(*fn_column_blob)(sqlite3_stmt *, int);
typedef const unsigned char *(*fn_column_text)(sqlite3_stmt *, int);
typedef int (*fn_column_bytes)(sqlite3_stmt *, int);
typedef int (*fn_column_int)(sqlite3_stmt *, int);
typedef int64_t (*fn_column_int64)(sqlite3_stmt *, int);

static struct {
  int init_done;
  int available;
  void *dl;
  fn_open_v2 open_v2;
  fn_close_v2 close_v2;
  fn_exec exec;
  fn_prepare_v2 prepare_v2;
  fn_step step;
  fn_finalize finalize;
  fn_bind_text bind_text;
  fn_bind_blob bind_blob;
  fn_bind_int bind_int;
  fn_bind_int64 bind_int64;
  fn_column_blob column_blob;
  fn_column_text column_text;
  fn_column_bytes column_bytes;
  fn_column_int column_int;
  fn_column_int64 column_int64;
} g_sq;

static sqlite3 *g_db = NULL;
static char g_db_dir[4096];
static int g_atexit_registered = 0;

static void storage_atexit_close(void) {
  fv_storage_close();
}

static int load_sqlite_once(void) {
  if (g_sq.init_done) return g_sq.available;
  g_sq.init_done = 1;
  g_sq.available = 0;

#ifdef __APPLE__
  const char *candidates[] = {
    "/usr/lib/libsqlite3.dylib",
    "/opt/homebrew/opt/sqlite/lib/libsqlite3.dylib",
    "libsqlite3.dylib",
    NULL
  };
#elif defined(_WIN32)
  const char *candidates[] = {
    "sqlite3.dll",
    "winsqlite3.dll",
    NULL
  };
#else
  const char *candidates[] = {
    "libsqlite3.so.0",
    "libsqlite3.so",
    NULL
  };
#endif

  void *h = NULL;
  for (int i = 0; candidates[i] != NULL; i++) {
    h = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
    if (h) break;
  }
  if (!h) return 0;

  g_sq.dl = h;
  g_sq.open_v2      = (fn_open_v2)dlsym(h, "sqlite3_open_v2");
  g_sq.close_v2     = (fn_close_v2)dlsym(h, "sqlite3_close_v2");
  if (!g_sq.close_v2) g_sq.close_v2 = (fn_close_v2)dlsym(h, "sqlite3_close");
  g_sq.exec         = (fn_exec)dlsym(h, "sqlite3_exec");
  g_sq.prepare_v2   = (fn_prepare_v2)dlsym(h, "sqlite3_prepare_v2");
  g_sq.step         = (fn_step)dlsym(h, "sqlite3_step");
  g_sq.finalize     = (fn_finalize)dlsym(h, "sqlite3_finalize");
  g_sq.bind_text    = (fn_bind_text)dlsym(h, "sqlite3_bind_text");
  g_sq.bind_blob    = (fn_bind_blob)dlsym(h, "sqlite3_bind_blob");
  g_sq.bind_int     = (fn_bind_int)dlsym(h, "sqlite3_bind_int");
  g_sq.bind_int64   = (fn_bind_int64)dlsym(h, "sqlite3_bind_int64");
  g_sq.column_blob  = (fn_column_blob)dlsym(h, "sqlite3_column_blob");
  g_sq.column_text  = (fn_column_text)dlsym(h, "sqlite3_column_text");
  g_sq.column_bytes = (fn_column_bytes)dlsym(h, "sqlite3_column_bytes");
  g_sq.column_int   = (fn_column_int)dlsym(h, "sqlite3_column_int");
  g_sq.column_int64 = (fn_column_int64)dlsym(h, "sqlite3_column_int64");

  if (!g_sq.open_v2 || !g_sq.close_v2 || !g_sq.exec || !g_sq.prepare_v2 ||
      !g_sq.step || !g_sq.finalize || !g_sq.bind_text || !g_sq.bind_blob ||
      !g_sq.bind_int || !g_sq.bind_int64 || !g_sq.column_blob ||
      !g_sq.column_text || !g_sq.column_bytes || !g_sq.column_int ||
      !g_sq.column_int64) {
    dlclose(h);
    g_sq.dl = NULL;
    return 0;
  }

  g_sq.available = 1;
  return 1;
}

typedef struct {
  int32_t len;
  uint8_t *data;
} FvBuf;

static FvBuf *fv_buf_alloc(const void *src, int32_t len) {
  if (len < 0) return NULL;
  FvBuf *b = (FvBuf *)malloc(sizeof(FvBuf));
  if (!b) return NULL;
  b->len = len;
  if (len == 0) {
    b->data = (uint8_t *)malloc(1);
    if (!b->data) { free(b); return NULL; }
    b->data[0] = 0;
  } else {
    b->data = (uint8_t *)malloc((size_t)len);
    if (!b->data) { free(b); return NULL; }
    if (src) memcpy(b->data, src, (size_t)len);
  }
  return b;
}

int32_t fv_storage_buf_len(const void *buf) {
  if (!buf) return -1;
  return ((const FvBuf *)buf)->len;
}

const void *fv_storage_buf_data(const void *buf) {
  if (!buf) return NULL;
  return ((const FvBuf *)buf)->data;
}

void fv_storage_buf_free(void *buf) {
  if (!buf) return;
  FvBuf *b = (FvBuf *)buf;
  free(b->data);
  free(b);
}

static int mkdir_p(const char *path) {
  if (!path || !path[0]) return -1;
  char tmp[4096];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) return -1;
  memcpy(tmp, path, len + 1);
  if (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[len - 1] = '\0';
  char *start = tmp + 1;
#ifdef _WIN32
  if (len >= 2 && tmp[1] == ':') {
    start = tmp + 2;
    if (*start == '/' || *start == '\\') start++;
  }
#endif
  for (char *p = start; *p; p++) {
    if (*p == '/' || *p == '\\') {
      char saved = *p;
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
      *p = saved;
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static const char *k_schema_sql =
  "PRAGMA journal_mode=WAL;"
  "PRAGMA synchronous=NORMAL;"
  "PRAGMA busy_timeout=5000;"
  "CREATE TABLE IF NOT EXISTS idb_kv ("
  "  partition_key TEXT NOT NULL,"
  "  origin TEXT NOT NULL,"
  "  store_key TEXT NOT NULL,"
  "  val_blob BLOB NOT NULL,"
  "  updated_at INTEGER NOT NULL,"
  "  PRIMARY KEY (partition_key, origin, store_key)"
  ");"
  "CREATE TABLE IF NOT EXISTS http_cache ("
  "  partition_key TEXT NOT NULL,"
  "  url TEXT NOT NULL,"
  "  status INTEGER NOT NULL,"
  "  final_url TEXT NOT NULL,"
  "  content_type TEXT NOT NULL,"
  "  etag TEXT NOT NULL,"
  "  last_modified TEXT NOT NULL,"
  "  vary TEXT NOT NULL,"
  "  expires_at INTEGER NOT NULL,"
  "  stored_at INTEGER NOT NULL,"
  "  last_access INTEGER NOT NULL,"
  "  body BLOB NOT NULL,"
  "  PRIMARY KEY (partition_key, url)"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_http_cache_lru ON http_cache(partition_key, last_access);"
  "CREATE TABLE IF NOT EXISTS permissions ("
  "  partition_key TEXT NOT NULL,"
  "  origin TEXT NOT NULL,"
  "  name TEXT NOT NULL,"
  "  state TEXT NOT NULL,"
  "  updated_at INTEGER NOT NULL,"
  "  PRIMARY KEY (partition_key, origin, name)"
  ");";

int32_t fv_storage_available(void) {
  return load_sqlite_once();
}

int64_t fv_storage_now_unix(void) {
  return (int64_t)time(NULL);
}

void fv_storage_close(void) {
  if (g_db && g_sq.close_v2) {
    g_sq.close_v2(g_db);
  }
  g_db = NULL;
  g_db_dir[0] = '\0';
}

static sqlite3 *ensure_db(const void *dir, int32_t dir_len) {
  if (!load_sqlite_once()) return NULL;
  if (!dir || dir_len <= 0 || dir_len >= (int32_t)sizeof(g_db_dir) - 32) return NULL;

  char dpath[4096];
  memcpy(dpath, dir, (size_t)dir_len);
  dpath[dir_len] = '\0';
  while (dir_len > 1 && dpath[dir_len - 1] == '/') {
    dpath[--dir_len] = '\0';
  }

  if (g_db && strcmp(g_db_dir, dpath) == 0) {
    return g_db;
  }

  fv_storage_close();
  if (mkdir_p(dpath) != 0) return NULL;

  char db_file[4096];
  int n = snprintf(db_file, sizeof(db_file), "%s/storage.sqlite", dpath);
  if (n <= 0 || (size_t)n >= sizeof(db_file)) return NULL;

  sqlite3 *db = NULL;
  int rc = g_sq.open_v2(db_file, &db,
                        FV_SQLITE_OPEN_READWRITE | FV_SQLITE_OPEN_CREATE | FV_SQLITE_OPEN_FULLMUTEX,
                        NULL);
  if (rc != FV_SQLITE_OK || !db) {
    if (db) g_sq.close_v2(db);
    return NULL;
  }

  rc = g_sq.exec(db, k_schema_sql, NULL, NULL, NULL);
  if (rc != FV_SQLITE_OK) {
    g_sq.close_v2(db);
    return NULL;
  }

  g_db = db;
  memcpy(g_db_dir, dpath, (size_t)dir_len + 1);
  if (!g_atexit_registered) {
    g_atexit_registered = 1;
    atexit(storage_atexit_close);
  }
  return g_db;
}

int32_t fv_storage_open(const void *profile_dir, int32_t dir_len) {
  return ensure_db(profile_dir, dir_len) != NULL ? 1 : 0;
}

int32_t fv_storage_is_wal(const void *profile_dir, int32_t dir_len) {
  sqlite3 *db = ensure_db(profile_dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  if (g_sq.prepare_v2(db, "PRAGMA journal_mode;", -1, &st, NULL) != FV_SQLITE_OK) return 0;
  int is_wal = 0;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    const unsigned char *txt = g_sq.column_text(st, 0);
    if (txt && (strcmp((const char *)txt, "wal") == 0 || strcmp((const char *)txt, "WAL") == 0)) {
      is_wal = 1;
    }
  }
  g_sq.finalize(st);
  return is_wal;
}

int32_t fv_storage_remove_db(const void *profile_dir, int32_t dir_len) {
  if (!profile_dir || dir_len <= 0 || dir_len >= 4000) return 0;
  char dpath[4096];
  memcpy(dpath, profile_dir, (size_t)dir_len);
  dpath[dir_len] = '\0';
  while (dir_len > 1 && dpath[dir_len - 1] == '/') dpath[--dir_len] = '\0';

  if (g_db && strcmp(g_db_dir, dpath) == 0) {
    fv_storage_close();
  }
  char p[4096];
  snprintf(p, sizeof(p), "%s/storage.sqlite", dpath);     unlink(p);
  snprintf(p, sizeof(p), "%s/storage.sqlite-wal", dpath); unlink(p);
  snprintf(p, sizeof(p), "%s/storage.sqlite-shm", dpath); unlink(p);
  rmdir(dpath);
  return 1;
}

/* Helper: safe non-null pointer for zero-length binds */
static const char k_empty_str[1] = {0};
static const char *safe_ptr(const void *p, int32_t len) {
  return (p && len > 0) ? (const char *)p : k_empty_str;
}

/* Big-endian encoding helpers */
static void put_u32_be(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)((v >> 24) & 0xFF);
  dst[1] = (uint8_t)((v >> 16) & 0xFF);
  dst[2] = (uint8_t)((v >> 8) & 0xFF);
  dst[3] = (uint8_t)(v & 0xFF);
}

static void put_i64_be(uint8_t *dst, int64_t v) {
  uint64_t u = (uint64_t)v;
  dst[0] = (uint8_t)((u >> 56) & 0xFF);
  dst[1] = (uint8_t)((u >> 48) & 0xFF);
  dst[2] = (uint8_t)((u >> 40) & 0xFF);
  dst[3] = (uint8_t)((u >> 32) & 0xFF);
  dst[4] = (uint8_t)((u >> 24) & 0xFF);
  dst[5] = (uint8_t)((u >> 16) & 0xFF);
  dst[6] = (uint8_t)((u >> 8) & 0xFF);
  dst[7] = (uint8_t)(u & 0xFF);
}

static uint32_t read_u32_be(const uint8_t *src) {
  return ((uint32_t)src[0] << 24) |
         ((uint32_t)src[1] << 16) |
         ((uint32_t)src[2] << 8) |
         (uint32_t)src[3];
}

/* =========================================================================
 * Table 1: idb_kv
 * ========================================================================= */

void *fv_idb_get(const void *dir, int32_t dir_len,
                 const void *part, int32_t part_len,
                 const void *origin, int32_t origin_len,
                 const void *key, int32_t key_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return NULL;
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT val_blob FROM idb_kv WHERE partition_key=?1 AND origin=?2 AND store_key=?3;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return NULL;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 3, safe_ptr(key, key_len), key_len, FV_SQLITE_TRANSIENT);
  FvBuf *out = NULL;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    int n = g_sq.column_bytes(st, 0);
    const void *blob = g_sq.column_blob(st, 0);
    out = fv_buf_alloc(blob, n);
  }
  g_sq.finalize(st);
  return out;
}

int32_t fv_idb_put(const void *dir, int32_t dir_len,
                   const void *part, int32_t part_len,
                   const void *origin, int32_t origin_len,
                   const void *key, int32_t key_len,
                   const void *val, int32_t val_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db || val_len < 0) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql =
    "INSERT INTO idb_kv (partition_key, origin, store_key, val_blob, updated_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5) "
    "ON CONFLICT(partition_key, origin, store_key) DO UPDATE SET "
    "val_blob=excluded.val_blob, updated_at=excluded.updated_at;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 3, safe_ptr(key, key_len), key_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_blob(st, 4, safe_ptr(val, val_len), val_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_int64(st, 5, (int64_t)time(NULL));
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


int32_t fv_idb_del(const void *dir, int32_t dir_len,
                   const void *part, int32_t part_len,
                   const void *origin, int32_t origin_len,
                   const void *key, int32_t key_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "DELETE FROM idb_kv WHERE partition_key=?1 AND origin=?2 AND store_key=?3;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 3, safe_ptr(key, key_len), key_len, FV_SQLITE_TRANSIENT);
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


void *fv_idb_list_keys(const void *dir, int32_t dir_len,
                       const void *part, int32_t part_len,
                       const void *origin, int32_t origin_len,
                       const void *prefix, int32_t prefix_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return NULL;
  sqlite3_stmt *st = NULL;
  const char *sql =
    "SELECT store_key FROM idb_kv WHERE partition_key=?1 AND origin=?2 ORDER BY store_key ASC;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return NULL;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);

  size_t cap = 256, len = 0;
  uint8_t *acc = (uint8_t *)malloc(cap);
  if (!acc) { g_sq.finalize(st); return NULL; }

  while (g_sq.step(st) == FV_SQLITE_ROW) {
    int klen = g_sq.column_bytes(st, 0);
    const unsigned char *kptr = g_sq.column_text(st, 0);
    if (!kptr || klen < 0) continue;
    if (prefix_len > 0) {
      if (klen < prefix_len || memcmp(kptr, prefix, (size_t)prefix_len) != 0) continue;
    }
    size_t need = len + 4 + (size_t)klen;
    if (need > cap) {
      while (need > cap) cap *= 2;
      uint8_t *na = (uint8_t *)realloc(acc, cap);
      if (!na) { free(acc); g_sq.finalize(st); return NULL; }
      acc = na;
    }
    put_u32_be(acc + len, (uint32_t)klen);
    len += 4;
    if (klen > 0) {
      memcpy(acc + len, kptr, (size_t)klen);
      len += (size_t)klen;
    }
  }
  g_sq.finalize(st);

  FvBuf *out = fv_buf_alloc(acc, (int32_t)len);
  free(acc);
  return out;
}

int32_t fv_idb_clear_origin(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len,
                            const void *origin, int32_t origin_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "DELETE FROM idb_kv WHERE partition_key=?1 AND origin=?2;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


int64_t fv_idb_usage_bytes(const void *dir, int32_t dir_len,
                           const void *part, int32_t part_len,
                           const void *origin, int32_t origin_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT COALESCE(SUM(LENGTH(val_blob)), 0) FROM idb_kv WHERE partition_key=?1 AND origin=?2;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  int64_t total = 0;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    total = g_sq.column_int64(st, 0);
  }
  g_sq.finalize(st);
  return total;
}

/* =========================================================================
 * Table 2: http_cache
 * ========================================================================= */

void *fv_http_cache_get(const void *dir, int32_t dir_len,
                        const void *part, int32_t part_len,
                        const void *url, int32_t url_len,
                        int64_t now_ts) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return NULL;
  sqlite3_stmt *st = NULL;
  const char *sql =
    "SELECT status, final_url, content_type, etag, last_modified, vary, "
    "       expires_at, stored_at, last_access, body "
    "FROM http_cache WHERE partition_key=?1 AND url=?2;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return NULL;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(url, url_len), url_len, FV_SQLITE_TRANSIENT);

  FvBuf *out = NULL;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    int32_t status = g_sq.column_int(st, 0);
    const void *furl = g_sq.column_text(st, 1); int32_t furl_n = g_sq.column_bytes(st, 1);
    const void *ct   = g_sq.column_text(st, 2); int32_t ct_n   = g_sq.column_bytes(st, 2);
    const void *etag = g_sq.column_text(st, 3); int32_t etag_n = g_sq.column_bytes(st, 3);
    const void *lm   = g_sq.column_text(st, 4); int32_t lm_n   = g_sq.column_bytes(st, 4);
    const void *vary = g_sq.column_text(st, 5); int32_t vary_n = g_sq.column_bytes(st, 5);
    int64_t expires_at  = g_sq.column_int64(st, 6);
    int64_t stored_at   = g_sq.column_int64(st, 7);
    int64_t last_access = g_sq.column_int64(st, 8);
    const void *body = g_sq.column_blob(st, 9); int32_t body_n = g_sq.column_bytes(st, 9);

    size_t total = 4 + 8 + 8 + 8 +
                   4 + (size_t)furl_n +
                   4 + (size_t)ct_n +
                   4 + (size_t)etag_n +
                   4 + (size_t)lm_n +
                   4 + (size_t)vary_n +
                   4 + (size_t)body_n;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (buf) {
      size_t o = 0;
      put_u32_be(buf + o, (uint32_t)status); o += 4;
      put_i64_be(buf + o, expires_at);       o += 8;
      put_i64_be(buf + o, stored_at);        o += 8;
      put_i64_be(buf + o, last_access);      o += 8;

      put_u32_be(buf + o, (uint32_t)furl_n); o += 4;
      if (furl_n > 0 && furl) { memcpy(buf + o, furl, (size_t)furl_n); o += (size_t)furl_n; }

      put_u32_be(buf + o, (uint32_t)ct_n); o += 4;
      if (ct_n > 0 && ct) { memcpy(buf + o, ct, (size_t)ct_n); o += (size_t)ct_n; }

      put_u32_be(buf + o, (uint32_t)etag_n); o += 4;
      if (etag_n > 0 && etag) { memcpy(buf + o, etag, (size_t)etag_n); o += (size_t)etag_n; }

      put_u32_be(buf + o, (uint32_t)lm_n); o += 4;
      if (lm_n > 0 && lm) { memcpy(buf + o, lm, (size_t)lm_n); o += (size_t)lm_n; }

      put_u32_be(buf + o, (uint32_t)vary_n); o += 4;
      if (vary_n > 0 && vary) { memcpy(buf + o, vary, (size_t)vary_n); o += (size_t)vary_n; }

      put_u32_be(buf + o, (uint32_t)body_n); o += 4;
      if (body_n > 0 && body) { memcpy(buf + o, body, (size_t)body_n); o += (size_t)body_n; }

      out = fv_buf_alloc(buf, (int32_t)total);
      free(buf);
    }
  }
  g_sq.finalize(st);

  /* Update last_access for LRU tracking */
  if (out) {
    sqlite3_stmt *up = NULL;
    if (g_sq.prepare_v2(db, "UPDATE http_cache SET last_access=?1 WHERE partition_key=?2 AND url=?3;", -1, &up, NULL) == FV_SQLITE_OK) {
      g_sq.bind_int64(up, 1, now_ts);
      g_sq.bind_text(up, 2, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
      g_sq.bind_text(up, 3, safe_ptr(url, url_len), url_len, FV_SQLITE_TRANSIENT);
      g_sq.step(up);
      g_sq.finalize(up);
    }
  }
  return out;
}

static void http_cache_evict_lru(sqlite3 *db, const void *part, int32_t part_len, int64_t max_bytes) {
  if (max_bytes <= 0) return;
  sqlite3_stmt *sum_st = NULL;
  if (g_sq.prepare_v2(db, "SELECT COALESCE(SUM(LENGTH(body)), 0) FROM http_cache WHERE partition_key=?1;", -1, &sum_st, NULL) != FV_SQLITE_OK) {
    return;
  }
  g_sq.bind_text(sum_st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  int64_t total = 0;
  if (g_sq.step(sum_st) == FV_SQLITE_ROW) {
    total = g_sq.column_int64(sum_st, 0);
  }
  g_sq.finalize(sum_st);
  if (total <= max_bytes) return;

  /* Collect URLs to evict in ascending last_access order */
  sqlite3_stmt *sel = NULL;
  const char *sel_sql =
    "SELECT url, LENGTH(body) FROM http_cache WHERE partition_key=?1 ORDER BY last_access ASC, stored_at ASC, url ASC;";
  if (g_sq.prepare_v2(db, sel_sql, -1, &sel, NULL) != FV_SQLITE_OK) return;
  g_sq.bind_text(sel, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);

  char **victims = NULL;
  int32_t *vlens = NULL;
  int nvic = 0, cap = 0;
  while (total > max_bytes && g_sq.step(sel) == FV_SQLITE_ROW) {
    const unsigned char *u = g_sq.column_text(sel, 0);
    int ulen = g_sq.column_bytes(sel, 0);
    int64_t bsz = g_sq.column_int64(sel, 1);
    if (!u || ulen < 0) continue;
    if (nvic == cap) {
      cap = cap ? cap * 2 : 16;
      char **nv = (char **)realloc(victims, (size_t)cap * sizeof(char *));
      int32_t *nl = (int32_t *)realloc(vlens, (size_t)cap * sizeof(int32_t));
      if (!nv || !nl) {
        if (nv) victims = nv;
        if (nl) vlens = nl;
        break;
      }
      victims = nv;
      vlens = nl;
    }
    victims[nvic] = (char *)malloc((size_t)ulen + 1);
    if (!victims[nvic]) break;
    memcpy(victims[nvic], u, (size_t)ulen);
    victims[nvic][ulen] = '\0';
    vlens[nvic] = (int32_t)ulen;
    nvic++;
    total -= bsz;
  }
  g_sq.finalize(sel);

  if (nvic > 0) {
    sqlite3_stmt *del = NULL;
    if (g_sq.prepare_v2(db, "DELETE FROM http_cache WHERE partition_key=?1 AND url=?2;", -1, &del, NULL) == FV_SQLITE_OK) {
      for (int i = 0; i < nvic; i++) {
        g_sq.bind_text(del, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
        g_sq.bind_text(del, 2, victims[i], vlens[i], FV_SQLITE_TRANSIENT);
        g_sq.step(del);
        g_sq.finalize(del);
        del = NULL;
        if (i + 1 < nvic) {
          if (g_sq.prepare_v2(db, "DELETE FROM http_cache WHERE partition_key=?1 AND url=?2;", -1, &del, NULL) != FV_SQLITE_OK) {
            break;
          }
        }
      }
      if (del) g_sq.finalize(del);
    }
  }
  for (int i = 0; i < nvic; i++) free(victims[i]);
  free(victims);
  free(vlens);
}

int32_t fv_http_cache_put(const void *dir, int32_t dir_len,
                          const void *part, int32_t part_len,
                          const void *meta, int32_t meta_len,
                          int32_t status,
                          int64_t expires_at,
                          int64_t stored_at,
                          const void *body, int32_t body_len,
                          int64_t max_bytes) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db || !meta || meta_len < 24 || body_len < 0) return 0;
  if (body_len > FV_HTTP_CACHE_MAX_ENTRY_BYTES) return 0;
  if (max_bytes > 0 && (int64_t)body_len > max_bytes) return 0;

  /* Unpack 6 length-prefixed strings from meta: url, final_url, ct, etag, lm, vary */
  const uint8_t *m = (const uint8_t *)meta;
  const char *fields[6];
  int32_t flens[6];
  int32_t pos = 0;
  for (int i = 0; i < 6; i++) {
    if (pos + 4 > meta_len) return 0;
    uint32_t fl = read_u32_be(m + pos);
    pos += 4;
    if (fl > (uint32_t)(meta_len - pos)) return 0;
    fields[i] = (const char *)(m + pos);
    flens[i] = (int32_t)fl;
    pos += (int32_t)fl;
  }

  sqlite3_stmt *st = NULL;
  const char *sql =
    "INSERT INTO http_cache "
    "(partition_key, url, status, final_url, content_type, etag, last_modified, vary, expires_at, stored_at, last_access, body) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
    "ON CONFLICT(partition_key, url) DO UPDATE SET "
    "status=excluded.status, final_url=excluded.final_url, content_type=excluded.content_type, "
    "etag=excluded.etag, last_modified=excluded.last_modified, vary=excluded.vary, "
    "expires_at=excluded.expires_at, stored_at=excluded.stored_at, last_access=excluded.last_access, body=excluded.body;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;

  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(fields[0], flens[0]), flens[0], FV_SQLITE_TRANSIENT);
  g_sq.bind_int(st, 3, status);
  g_sq.bind_text(st, 4, safe_ptr(fields[1], flens[1]), flens[1], FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 5, safe_ptr(fields[2], flens[2]), flens[2], FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 6, safe_ptr(fields[3], flens[3]), flens[3], FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 7, safe_ptr(fields[4], flens[4]), flens[4], FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 8, safe_ptr(fields[5], flens[5]), flens[5], FV_SQLITE_TRANSIENT);
  g_sq.bind_int64(st, 9, expires_at);
  g_sq.bind_int64(st, 10, stored_at);
  g_sq.bind_int64(st, 11, stored_at);
  g_sq.bind_blob(st, 12, safe_ptr(body, body_len), body_len, FV_SQLITE_TRANSIENT);

  int rc = g_sq.step(st);
  g_sq.finalize(st);
  if (rc != FV_SQLITE_DONE) return 0;

  if (max_bytes > 0) {
    http_cache_evict_lru(db, part, part_len, max_bytes);
  }
  return 1;
}

int32_t fv_http_cache_touch(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len,
                            const void *url, int32_t url_len,
                            int64_t new_expires_at,
                            int64_t now_ts,
                            const void *etag, int32_t etag_len,
                            const void *lm, int32_t lm_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql =
    "UPDATE http_cache SET expires_at=?1, last_access=?2, "
    "etag=CASE WHEN LENGTH(?3)>0 THEN ?3 ELSE etag END, "
    "last_modified=CASE WHEN LENGTH(?4)>0 THEN ?4 ELSE last_modified END "
    "WHERE partition_key=?5 AND url=?6;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_int64(st, 1, new_expires_at);
  g_sq.bind_int64(st, 2, now_ts);
  g_sq.bind_text(st, 3, safe_ptr(etag, etag_len), etag_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 4, safe_ptr(lm, lm_len), lm_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 5, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 6, safe_ptr(url, url_len), url_len, FV_SQLITE_TRANSIENT);
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


int32_t fv_http_cache_delete(const void *dir, int32_t dir_len,
                             const void *part, int32_t part_len,
                             const void *url, int32_t url_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "DELETE FROM http_cache WHERE partition_key=?1 AND url=?2;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(url, url_len), url_len, FV_SQLITE_TRANSIENT);
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


int64_t fv_http_cache_size_bytes(const void *dir, int32_t dir_len,
                                 const void *part, int32_t part_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT COALESCE(SUM(LENGTH(body)), 0) FROM http_cache WHERE partition_key=?1;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  int64_t total = 0;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    total = g_sq.column_int64(st, 0);
  }
  g_sq.finalize(st);
  return total;
}

int32_t fv_http_cache_count(const void *dir, int32_t dir_len,
                            const void *part, int32_t part_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT COUNT(*) FROM http_cache WHERE partition_key=?1;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  int32_t cnt = 0;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    cnt = g_sq.column_int(st, 0);
  }
  g_sq.finalize(st);
  return cnt;
}

/* =========================================================================
 * Table 3: permissions
 * ========================================================================= */

void *fv_perm_get(const void *dir, int32_t dir_len,
                  const void *part, int32_t part_len,
                  const void *origin, int32_t origin_len,
                  const void *name, int32_t name_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return NULL;
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT state FROM permissions WHERE partition_key=?1 AND origin=?2 AND name=?3;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return NULL;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 3, safe_ptr(name, name_len), name_len, FV_SQLITE_TRANSIENT);
  FvBuf *out = NULL;
  if (g_sq.step(st) == FV_SQLITE_ROW) {
    int n = g_sq.column_bytes(st, 0);
    const unsigned char *txt = g_sq.column_text(st, 0);
    out = fv_buf_alloc(txt, n);
  }
  g_sq.finalize(st);
  return out;
}

int32_t fv_perm_set(const void *dir, int32_t dir_len,
                    const void *part, int32_t part_len,
                    const void *origin, int32_t origin_len,
                    const void *name, int32_t name_len,
                    const void *state, int32_t state_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql =
    "INSERT INTO permissions (partition_key, origin, name, state, updated_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5) "
    "ON CONFLICT(partition_key, origin, name) DO UPDATE SET "
    "state=excluded.state, updated_at=excluded.updated_at;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 3, safe_ptr(name, name_len), name_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 4, safe_ptr(state, state_len), state_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_int64(st, 5, (int64_t)time(NULL));
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}


int32_t fv_perm_clear(const void *dir, int32_t dir_len,
                      const void *part, int32_t part_len,
                      const void *origin, int32_t origin_len) {
  sqlite3 *db = ensure_db(dir, dir_len);
  if (!db) return 0;
  sqlite3_stmt *st = NULL;
  const char *sql = "DELETE FROM permissions WHERE partition_key=?1 AND origin=?2;";
  if (g_sq.prepare_v2(db, sql, -1, &st, NULL) != FV_SQLITE_OK) return 0;
  g_sq.bind_text(st, 1, safe_ptr(part, part_len), part_len, FV_SQLITE_TRANSIENT);
  g_sq.bind_text(st, 2, safe_ptr(origin, origin_len), origin_len, FV_SQLITE_TRANSIENT);
  int rc = g_sq.step(st);
  g_sq.finalize(st);
  return rc == FV_SQLITE_DONE ? 1 : 0;
}

/* =========================================================================
 * Loopback HTTP cache test server (/cached, /nostore, /etag)
 * ========================================================================= */

typedef struct {
  int srv_fd;
  int max_reqs;
} FvCacheHttpCfg;

static volatile int32_t g_test_http_hits = 0;

static void *fv_cache_http_thread(void *arg) {
  FvCacheHttpCfg *cfg = (FvCacheHttpCfg *)arg;
  int srv = cfg->srv_fd;
  int max_reqs = cfg->max_reqs;
  free(cfg);

  for (int i = 0; i < max_reqs; i++) {
    int c = accept(srv, NULL, NULL);
    if (c < 0) break;
    char req[4096];
    ssize_t n = recv(c, req, sizeof(req) - 1, 0);
    if (n > 0) {
      req[n] = '\0';
      __sync_fetch_and_add(&g_test_http_hits, 1);
      const char *resp = NULL;
      if (strstr(req, "GET /cached ")) {
        resp =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain\r\n"
          "Cache-Control: max-age=3600\r\n"
          "Content-Length: 9\r\n"
          "Connection: close\r\n\r\n"
          "cached-v1";
      } else if (strstr(req, "GET /nostore ")) {
        resp =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain\r\n"
          "Cache-Control: no-store\r\n"
          "Content-Length: 15\r\n"
          "Connection: close\r\n\r\n"
          "secret-no-store";
      } else if (strstr(req, "GET /etag ")) {
        if (strstr(req, "If-None-Match: \"v1\"") || strstr(req, "if-none-match: \"v1\"")) {
          resp =
            "HTTP/1.1 304 Not Modified\r\n"
            "Cache-Control: max-age=3600\r\n"
            "ETag: \"v1\"\r\n"
            "Connection: close\r\n\r\n";
        } else {
          resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Cache-Control: max-age=0\r\n"
            "ETag: \"v1\"\r\n"
            "Content-Length: 12\r\n"
            "Connection: close\r\n\r\n"
            "etag-body-v1";
        }
      } else {
        resp =
          "HTTP/1.1 404 Not Found\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      }
      if (resp) {
        size_t rlen = strlen(resp);
        ssize_t w = send(c, resp, rlen, 0);
        (void)w;
      }
    }
    fv_sock_close(c);
  }
  fv_sock_close(srv);
  return NULL;
}

int32_t fv_storage_test_http_start(int32_t max_reqs) {
  g_test_http_hits = 0;
#ifdef _WIN32
  fv_win_wsa_init();
#endif
  int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srv, 8) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  socklen_t alen = sizeof(a);
  if (getsockname(srv, (struct sockaddr *)&a, &alen) < 0) {
    fv_sock_close(srv);
    return -1;
  }
  int port = ntohs(a.sin_port);
  FvCacheHttpCfg *cfg = (FvCacheHttpCfg *)malloc(sizeof(FvCacheHttpCfg));
  if (!cfg) { fv_sock_close(srv); return -1; }
  cfg->srv_fd = srv;
  cfg->max_reqs = max_reqs;
  pthread_t tid;
  if (pthread_create(&tid, NULL, fv_cache_http_thread, cfg) != 0) {
    free(cfg);
    fv_sock_close(srv);
    return -1;
  }
  pthread_detach(tid);
  return port;
}

int32_t fv_storage_test_http_hits(void) {
  return g_test_http_hits;
}
