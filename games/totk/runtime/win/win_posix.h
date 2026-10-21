/* win_posix.h - POSIX pieces llvm-mingw lacks, force-included (-include) into the shared runtime sources on Windows.
 * Apple-only headers the shared sources include are shadowed by runtime/win/compat/ (same include path order).
 * Implementations: runtime/win/win_posix.c. */
#pragma once
#ifdef _WIN32
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define mkdir(path, mode) _mkdir(path)
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif

#define F_FULLFSYNC 51
static inline int fcntl(int fd, int cmd, ...) { return cmd == F_FULLFSYNC ? _commit(fd) : -1; }   /* the only use */
static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
static inline int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }

static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
#define timegm _mkgmtime
static inline ssize_t readlink(const char *path, char *buf, size_t n) { (void)path; (void)buf; (void)n; return -1; }

/* Darwin's one-argument pthread_setname_np (the calling thread) next to winpthreads' two-argument one. */
#define WIN_SETNAME_PICK(a, b, f, ...) f
#define pthread_setname_np(...) WIN_SETNAME_PICK(__VA_ARGS__, pthread_setname_np, win_setname_self, )(__VA_ARGS__)
static inline int win_setname_self(const char *name) { return (pthread_setname_np)(pthread_self(), name); }
/* No signal-driven longjmp recovery on Windows (guarded reads use host_fault.h): plain setjmp. */
#ifndef sigsetjmp
#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp(env, v) longjmp(env, v)
#endif

typedef jmp_buf sigjmp_buf;
#include <signal.h>
#include <string.h>
#ifndef SIGBUS
#define SIGBUS 10
#endif
/* Signal masks (a self-check in hle_nvn.c reads them): nothing is ever blocked. */
#define sigset_t win_sigset_t
typedef unsigned long long win_sigset_t;
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#define sigprocmask(how, set, old) win_sigprocmask(how, set, old)
#define sigismember(set, sig) ((int)((*(set) >> (sig)) & 1))
static inline int win_sigprocmask(int how, const win_sigset_t *set, win_sigset_t *old) { (void)how; (void)set; if (old) *old = 0; return 0; }
static inline int getpagesize(void) { return 4096; }
static inline size_t strlcat(char *dst, const char *src, size_t cap) {
    size_t d = strnlen(dst, cap), s = strlen(src);
    if (d < cap) { size_t n = s < cap - d - 1 ? s : cap - d - 1; memcpy(dst + d, src, n); dst[d + n] = 0; }
    return d + s;
}
static inline size_t strlcpy(char *dst, const char *src, size_t cap) {
    size_t s = strlen(src);
    if (cap) { size_t n = s < cap - 1 ? s : cap - 1; memcpy(dst, src, n); dst[n] = 0; }
    return s;
}

ssize_t pread(int fd, void *buf, size_t n, off_t off);   /* positioned, thread safe (ReadFile + OVERLAPPED) */
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int fsync(int fd);
void arc4random_buf(void *buf, size_t n);
int win_pthread_threadid(uintptr_t thread, uint64_t *id);   /* thread == 0: caller (OS thread id) */
#define pthread_threadid_np(thread, id) win_pthread_threadid((uintptr_t)(thread), (id))
uint64_t clock_gettime_nsec_np(int clock);
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

#ifdef __cplusplus
}
#endif
#endif
