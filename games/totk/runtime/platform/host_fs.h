/* host_fs.h - files with UTF-8 paths (Windows converts to UTF-16 and uses \\?\ for long paths),
 * read-only mappings, advisory locks and per-user data directories.
 * Sizes/offsets are 64-bit. I/O functions return bytes transferred or -1. */
#ifndef HOST_FS_H
#define HOST_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct host_file host_file;

enum {
    HOST_FS_READ   = 1,
    HOST_FS_WRITE  = 2,
    HOST_FS_CREATE = 4,  /* create if missing */
    HOST_FS_TRUNC  = 8,  /* truncate existing */
    HOST_FS_EXCL   = 16, /* with CREATE: fail if it exists */
};

host_file *host_fs_open(const char *path, int flags); /* NULL on failure */
void host_fs_close(host_file *f);
int64_t host_fs_read(host_file *f, void *buf, size_t n);  /* sequential, from the current position */
int64_t host_fs_write(host_file *f, const void *buf, size_t n);
int64_t host_fs_pread(host_file *f, void *buf, size_t n, uint64_t off);  /* thread-safe; current position afterwards unspecified */
int64_t host_fs_pwrite(host_file *f, const void *buf, size_t n, uint64_t off);
int64_t host_fs_size(host_file *f);
int host_fs_truncate(host_file *f, uint64_t size);
int host_fs_flush(host_file *f); /* to stable storage */

/* Advisory whole-file lock: exclusive or shared; wait == 0 fails immediately when contended. */
int host_fs_lock(host_file *f, int exclusive, int wait);
int host_fs_unlock(host_file *f);

typedef struct host_mapping { void *addr; size_t size; void *base; size_t base_size; void *os; } host_mapping;
/* Map [off, off+len) read-only (len 0 = to end of file). Returns m->addr or NULL. */
void *host_fs_mmap_ro(host_file *f, uint64_t off, size_t len, host_mapping *m);
void host_fs_munmap(host_mapping *m);

typedef struct host_stat { int exists; int is_dir; uint64_t size; } host_stat;
int host_fs_stat(const char *path, host_stat *st); /* 0 with st->exists = 0 when missing */
int host_fs_mkdirs(const char *path);              /* mkdir -p */
int host_fs_remove(const char *path);              /* file or empty directory */
int host_fs_rename(const char *from, const char *to); /* replaces `to` */
/* Calls cb for each entry except . and ..; stops early when cb returns non-zero. */
int host_fs_list(const char *dir, int (*cb)(const char *name, int is_dir, void *ctx), void *ctx);

typedef enum { HOST_DIR_DATA, HOST_DIR_SAVES, HOST_DIR_CACHE, HOST_DIR_MODS, HOST_DIR_LOGS } host_dir;
/* Per-user directory for `game` (e.g. "TOTK"), UTF-8, no trailing separator, created if missing.
 *   macOS  : ~/Library/Application Support/MacNativeGames/<game>[/saves|/shader-cache|/mods],
 *            ~/Library/Logs/MacNativeGames
 *   Windows: %LOCALAPPDATA%\MacNativeGames\<game>\{saves,shader-cache,mods,logs}
 * Returns the string length or -1 (cap too small / no home). */
int host_fs_user_dir(host_dir which, const char *game, char *buf, size_t cap);
char host_fs_sep(void);

#ifdef __cplusplus
}
#endif
#endif
