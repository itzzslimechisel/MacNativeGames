/* host_fs_win.c - host_fs.h on Windows: UTF-8 paths -> UTF-16 (\\?\ long paths), CreateFileW,
 * file mappings, LockFileEx, %LOCALAPPDATA% user directories. */
#include "host_fs.h"
#include "host_win_internal.h"
#include <stdio.h>
#include <stdlib.h>

struct host_file { HANDLE h; };

wchar_t *host_win_widen(const char *utf8, int long_path) {
    if (!utf8) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    int abs_drive = long_path && ((utf8[0] | 0x20) >= 'a' && (utf8[0] | 0x20) <= 'z') && utf8[1] == ':' &&
                    (utf8[2] == '/' || utf8[2] == '\\');
    int pre = abs_drive ? 4 : 0;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n + pre));
    if (!w) return NULL;
    if (pre) memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w + pre, n);
    if (long_path)
        for (wchar_t *p = w + pre; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

int host_win_narrow(const wchar_t *w, char *buf, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)(cap > 0x7fffffff ? 0x7fffffff : cap), NULL, NULL);
    return n > 0 ? n - 1 : -1;
}

host_file *host_fs_open(const char *path, int flags) {
    DWORD access = 0, disp;
    if (flags & HOST_FS_READ) access |= GENERIC_READ;
    if (flags & HOST_FS_WRITE) access |= GENERIC_WRITE;
    if ((flags & HOST_FS_CREATE) && (flags & HOST_FS_EXCL)) disp = CREATE_NEW;
    else if ((flags & HOST_FS_CREATE) && (flags & HOST_FS_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & HOST_FS_CREATE) disp = OPEN_ALWAYS;
    else if (flags & HOST_FS_TRUNC) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;
    wchar_t *w = host_win_widen(path, 1);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    host_file *f = (host_file *)malloc(sizeof *f);
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h;
    return f;
}

void host_fs_close(host_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

#define CHUNK 0x40000000u

int64_t host_fs_read(host_file *f, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)(n - done > CHUNK ? CHUNK : n - done), got = 0;
        if (!ReadFile(f->h, (char *)buf + done, want, &got, NULL)) return done ? (int64_t)done : -1;
        done += got;
        if (got < want) break;
    }
    return (int64_t)done;
}

int64_t host_fs_write(host_file *f, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)(n - done > CHUNK ? CHUNK : n - done), put = 0;
        if (!WriteFile(f->h, (const char *)buf + done, want, &put, NULL)) return done ? (int64_t)done : -1;
        done += put;
        if (put < want) break;
    }
    return (int64_t)done;
}

int64_t host_fs_pread(host_file *f, void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t o = off + done;
        ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD want = (DWORD)(n - done > CHUNK ? CHUNK : n - done), got = 0;
        if (!ReadFile(f->h, (char *)buf + done, want, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (int64_t)done : -1;
        }
        done += got;
        if (got < want) break;
    }
    return (int64_t)done;
}

int64_t host_fs_pwrite(host_file *f, const void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t o = off + done;
        ov.Offset = (DWORD)o; ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD want = (DWORD)(n - done > CHUNK ? CHUNK : n - done), put = 0;
        if (!WriteFile(f->h, (const char *)buf + done, want, &put, &ov)) return done ? (int64_t)done : -1;
        done += put;
        if (put < want) break;
    }
    return (int64_t)done;
}

int64_t host_fs_size(host_file *f) {
    LARGE_INTEGER s;
    return GetFileSizeEx(f->h, &s) ? (int64_t)s.QuadPart : -1;
}

int host_fs_truncate(host_file *f, uint64_t size) {
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e) ? 0 : -1;
}

int host_fs_flush(host_file *f) { return FlushFileBuffers(f->h) ? 0 : -1; }

int host_fs_lock(host_file *f, int exclusive, int wait) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    DWORD fl = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    return LockFileEx(f->h, fl, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
}

int host_fs_unlock(host_file *f) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    return UnlockFileEx(f->h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
}

void *host_fs_mmap_ro(host_file *f, uint64_t off, size_t len, host_mapping *m) {
    memset(m, 0, sizeof *m);
    int64_t fsz = host_fs_size(f);
    if (fsz < 0 || off > (uint64_t)fsz) return NULL;
    if (!len) len = (size_t)((uint64_t)fsz - off);
    if (!len || off + len > (uint64_t)fsz) return NULL;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t base = off & ~(uint64_t)(si.dwAllocationGranularity - 1);
    HANDLE map = CreateFileMappingW(f->h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map) return NULL;
    size_t bsz = (size_t)(off - base) + len;
    void *v = MapViewOfFile(map, FILE_MAP_READ, (DWORD)(base >> 32), (DWORD)base, bsz);
    CloseHandle(map); /* the view keeps the section alive */
    if (!v) return NULL;
    m->base = v; m->base_size = bsz;
    m->addr = (char *)v + (off - base); m->size = len;
    return m->addr;
}

void host_fs_munmap(host_mapping *m) {
    if (m && m->base) UnmapViewOfFile(m->base);
    if (m) memset(m, 0, sizeof *m);
}

int host_fs_stat(const char *path, host_stat *st) {
    memset(st, 0, sizeof *st);
    wchar_t *w = host_win_widen(path, 1);
    if (!w) return -1;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    DWORD err = ok ? 0 : GetLastError();
    free(w);
    if (!ok) return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    st->exists = 1;
    st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return 0;
}

static int mkdir1(const char *p) {
    wchar_t *w = host_win_widen(p, 1);
    if (!w) return -1;
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD err = ok ? 0 : GetLastError();
    free(w);
    return (ok || err == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

int host_fs_mkdirs(const char *path) {
    size_t n = strlen(path);
    char *tmp = (char *)malloc(n + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, n + 1);
    int rc = 0;
    for (size_t i = 1; i <= n && rc == 0; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            size_t l = strlen(tmp);
            if (l && tmp[l - 1] != ':' && !(l == 1 && (tmp[0] == '/' || tmp[0] == '\\'))) rc = mkdir1(tmp);
            tmp[i] = c;
        }
    }
    free(tmp);
    return rc;
}

int host_fs_remove(const char *path) {
    wchar_t *w = host_win_widen(path, 1);
    if (!w) return -1;
    DWORD a = GetFileAttributesW(w);
    BOOL ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryW(w) : DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int host_fs_rename(const char *from, const char *to) {
    wchar_t *a = host_win_widen(from, 1), *b = host_win_widen(to, 1);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a); free(b);
    return ok ? 0 : -1;
}

int host_fs_list(const char *dir, int (*cb)(const char *name, int is_dir, void *ctx), void *ctx) {
    size_t n = strlen(dir);
    char *pat = (char *)malloc(n + 3);
    if (!pat) return -1;
    memcpy(pat, dir, n);
    size_t k = n;
    if (k && dir[k - 1] != '/' && dir[k - 1] != '\\') pat[k++] = '\\';
    pat[k++] = '*'; pat[k] = 0;
    wchar_t *w = host_win_widen(pat, 1);
    free(pat);
    if (!w) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    char name[MAX_PATH * 4];
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (host_win_narrow(fd.cFileName, name, sizeof name) < 0) continue;
        if (cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

char host_fs_sep(void) { return '\\'; }

int host_fs_user_dir(host_dir which, const char *game, char *buf, size_t cap) {
    wchar_t wbase[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", wbase, MAX_PATH);
    if (!n || n >= MAX_PATH) return -1;
    char base[MAX_PATH * 4];
    if (host_win_narrow(wbase, base, sizeof base) < 0) return -1;
    static const char *const sub[] = {"", "\\saves", "\\shader-cache", "\\mods", "\\logs"};
    if ((unsigned)which > HOST_DIR_LOGS) return -1;
    int len = snprintf(buf, cap, "%s\\MacNativeGames\\%s%s", base, game ? game : "TOTK", sub[which]);
    if (len < 0 || (size_t)len >= cap) return -1;
    if (host_fs_mkdirs(buf)) return -1;
    return len;
}
