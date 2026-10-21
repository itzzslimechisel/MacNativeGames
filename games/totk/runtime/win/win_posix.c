/* win_posix.c - see win_posix.h. */
#include "win_posix.h"
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <string.h>

static ssize_t positioned(int fd, void *buf, size_t n, off_t off, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (n > 0x7fffffff) n = 0x7fffffff;
    OVERLAPPED o = {0};
    o.Offset = (DWORD)((uint64_t)off & 0xffffffffu);
    o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buf, (DWORD)n, &done, &o) : ReadFile(h, buf, (DWORD)n, &done, &o);
    if (!ok) {
        if (!write && GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return positioned(fd, buf, n, off, 0); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return positioned(fd, (void *)buf, n, off, 1); }
int fsync(int fd) { return _commit(fd); }
void arc4random_buf(void *buf, size_t n) {
    if (BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) memset(buf, 0, n);
}
int win_pthread_threadid(uintptr_t thread, uint64_t *id) {
    if (!id) return EINVAL;
    *id = thread ? (uint64_t)GetThreadId(pthread_gethandle((pthread_t)thread)) : (uint64_t)GetCurrentThreadId();
    return 0;
}
uint64_t clock_gettime_nsec_np(int clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---------- compat/mach/mach_time.h ---------- */
#include "compat/mach/mach_time.h"
uint64_t mach_absolute_time(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
uint64_t mach_continuous_time(void) { return mach_absolute_time(); }
int mach_timebase_info(mach_timebase_info_data_t *info) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    uint64_t num = 1000000000ull, den = (uint64_t)f.QuadPart;
    for (uint64_t a = num, b = den; b;) { uint64_t r = a % b; a = b; b = r; if (!b) { num /= a; den /= a; } }
    info->numer = (uint32_t)num; info->denom = (uint32_t)den;
    return 0;
}

/* ---------- compat/sys/file.h ---------- */
#include "compat/sys/file.h"
int flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED o = {0};
    if (op & LOCK_UN) return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &o) ? 0 : -1;
    DWORD flags = (op & LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (op & LOCK_NB ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &o)) return 0;
    errno = GetLastError() == ERROR_LOCK_VIOLATION ? EWOULDBLOCK : EIO;
    return -1;
}

/* ---------- compat/sys/mman.h ---------- */
#include "compat/sys/mman.h"
static DWORD page_prot(int prot) {
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : prot & PROT_READ ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return prot & PROT_WRITE ? PAGE_READWRITE : prot & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (flags & MAP_ANON) {
        void *p = VirtualAlloc(flags & MAP_FIXED ? addr : NULL, len, MEM_RESERVE | MEM_COMMIT, page_prot(prot));
        return p ? p : MAP_FAILED;
    }
    if ((prot & PROT_WRITE) || (flags & MAP_FIXED)) { errno = ENOTSUP; return MAP_FAILED; }   /* read-only file views only */
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    HANDLE m = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) { errno = EACCES; return MAP_FAILED; }
    void *p = MapViewOfFile(m, FILE_MAP_READ, (DWORD)((uint64_t)off >> 32), (DWORD)((uint64_t)off & 0xffffffffu), len);
    CloseHandle(m);   /* the view keeps the mapping alive */
    return p ? p : MAP_FAILED;
}
int munmap(void *addr, size_t len) {
    (void)len;
    if (UnmapViewOfFile(addr)) return 0;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
int mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    if (prot == PROT_NONE) return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : -1;
    if (!VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot))) return -1;   /* commits reserved pages, keeps contents */
    return VirtualProtect(addr, len, page_prot(prot), &old) ? 0 : -1;
}

void mach_wait_until(uint64_t deadline) {
    for (;;) {
        uint64_t now = mach_absolute_time();
        if (now >= deadline) return;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        uint64_t ms = (deadline - now) * 1000 / (uint64_t)f.QuadPart;
        if (ms > 1) Sleep((DWORD)(ms - 1)); else SwitchToThread();
    }
}

/* ---------- compat/sys/resource.h ---------- */
#include "compat/sys/resource.h"
int getrusage(int who, struct rusage *ru) {
    (void)who;
    FILETIME c, e, k, u;
    memset(ru, 0, sizeof *ru);
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1;
    uint64_t ut = ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10, kt = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
    ru->ru_utime.tv_sec = (long)(ut / 1000000); ru->ru_utime.tv_usec = (long)(ut % 1000000);
    ru->ru_stime.tv_sec = (long)(kt / 1000000); ru->ru_stime.tv_usec = (long)(kt % 1000000);
    return 0;
}
