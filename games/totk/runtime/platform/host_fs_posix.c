/* host_fs_posix.c - host_fs.h on macOS/iOS (plain POSIX: open/pread/flock/mmap/opendir). */
#include "host_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct host_file { int fd; };

host_file *host_fs_open(const char *path, int flags) {
    int o = O_CLOEXEC;
    if ((flags & HOST_FS_READ) && (flags & HOST_FS_WRITE)) o |= O_RDWR;
    else if (flags & HOST_FS_WRITE) o |= O_WRONLY;
    else o |= O_RDONLY;
    if (flags & HOST_FS_CREATE) o |= O_CREAT;
    if (flags & HOST_FS_TRUNC) o |= O_TRUNC;
    if (flags & HOST_FS_EXCL) o |= O_EXCL;
    int fd = open(path, o, 0644);
    if (fd < 0) return NULL;
    host_file *f = (host_file *)malloc(sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

void host_fs_close(host_file *f) { if (f) { close(f->fd); free(f); } }

#define IO_LOOP(call)                                                   \
    size_t done = 0;                                                    \
    while (done < n) {                                                  \
        ssize_t r = (call);                                             \
        if (r < 0) { if (errno == EINTR) continue; return done ? (int64_t)done : -1; } \
        if (r == 0) break;                                              \
        done += (size_t)r;                                              \
    }                                                                   \
    return (int64_t)done;

int64_t host_fs_read(host_file *f, void *buf, size_t n) { IO_LOOP(read(f->fd, (char *)buf + done, n - done)) }
int64_t host_fs_write(host_file *f, const void *buf, size_t n) { IO_LOOP(write(f->fd, (const char *)buf + done, n - done)) }
int64_t host_fs_pread(host_file *f, void *buf, size_t n, uint64_t off) {
    IO_LOOP(pread(f->fd, (char *)buf + done, n - done, (off_t)(off + done)))
}
int64_t host_fs_pwrite(host_file *f, const void *buf, size_t n, uint64_t off) {
    IO_LOOP(pwrite(f->fd, (const char *)buf + done, n - done, (off_t)(off + done)))
}

int64_t host_fs_size(host_file *f) {
    struct stat st;
    return fstat(f->fd, &st) == 0 ? (int64_t)st.st_size : -1;
}
int host_fs_truncate(host_file *f, uint64_t size) { return ftruncate(f->fd, (off_t)size) == 0 ? 0 : -1; }
int host_fs_flush(host_file *f) { return fsync(f->fd) == 0 ? 0 : -1; }

int host_fs_lock(host_file *f, int exclusive, int wait) {
    return flock(f->fd, (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB)) == 0 ? 0 : -1;
}
int host_fs_unlock(host_file *f) { return flock(f->fd, LOCK_UN) == 0 ? 0 : -1; }

void *host_fs_mmap_ro(host_file *f, uint64_t off, size_t len, host_mapping *m) {
    memset(m, 0, sizeof *m);
    int64_t fsz = host_fs_size(f);
    if (fsz < 0 || off > (uint64_t)fsz) return NULL;
    if (!len) len = (size_t)((uint64_t)fsz - off);
    if (!len || off + len > (uint64_t)fsz) return NULL;
    uint64_t pg = (uint64_t)getpagesize(), base = off & ~(pg - 1);
    size_t bsz = (size_t)(off - base) + len;
    void *v = mmap(NULL, bsz, PROT_READ, MAP_PRIVATE, f->fd, (off_t)base);
    if (v == MAP_FAILED) return NULL;
    m->base = v; m->base_size = bsz;
    m->addr = (char *)v + (off - base); m->size = len;
    return m->addr;
}

void host_fs_munmap(host_mapping *m) {
    if (m && m->base) munmap(m->base, m->base_size);
    if (m) memset(m, 0, sizeof *m);
}

int host_fs_stat(const char *path, host_stat *st) {
    memset(st, 0, sizeof *st);
    struct stat s;
    if (stat(path, &s) != 0) return (errno == ENOENT || errno == ENOTDIR) ? 0 : -1;
    st->exists = 1;
    st->is_dir = S_ISDIR(s.st_mode) ? 1 : 0;
    st->size = (uint64_t)s.st_size;
    return 0;
}

int host_fs_mkdirs(const char *path) {
    size_t n = strlen(path);
    char *tmp = (char *)malloc(n + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, n + 1);
    int rc = 0;
    for (size_t i = 1; i <= n && rc == 0; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) rc = -1;
            tmp[i] = c;
        }
    }
    free(tmp);
    return rc;
}

int host_fs_remove(const char *path) { return remove(path) == 0 ? 0 : -1; }
int host_fs_rename(const char *from, const char *to) { return rename(from, to) == 0 ? 0 : -1; }

int host_fs_list(const char *dir, int (*cb)(const char *name, int is_dir, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (cb(e->d_name, e->d_type == DT_DIR, ctx)) break;
    }
    closedir(d);
    return 0;
}

char host_fs_sep(void) { return '/'; }

int host_fs_user_dir(host_dir which, const char *game, char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    if (!game) game = "TOTK";
    int len;
    switch (which) {
    case HOST_DIR_DATA: len = snprintf(buf, cap, "%s/Library/Application Support/MacNativeGames/%s", home, game); break;
    case HOST_DIR_SAVES: len = snprintf(buf, cap, "%s/Library/Application Support/MacNativeGames/%s/saves", home, game); break;
    case HOST_DIR_CACHE: len = snprintf(buf, cap, "%s/Library/Application Support/MacNativeGames/%s/shader-cache", home, game); break;
    case HOST_DIR_MODS: len = snprintf(buf, cap, "%s/Library/Application Support/MacNativeGames/%s/mods", home, game); break;
    case HOST_DIR_LOGS: len = snprintf(buf, cap, "%s/Library/Logs/MacNativeGames", home); break;
    default: return -1;
    }
    if (len < 0 || (size_t)len >= cap) return -1;
    if (host_fs_mkdirs(buf)) return -1;
    return len;
}
