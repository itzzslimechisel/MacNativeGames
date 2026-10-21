/* host_thread.h - host threads and synchronization (Windows: SRWLOCK/CONDITION_VARIABLE/INIT_ONCE/TLS,
 * POSIX: pthreads + Mach for suspend/context). Primitives are statically initializable with the *_INIT macros.
 * host_cpu_context is the portable register snapshot shared with host_fault.h. */
#ifndef HOST_THREAD_H
#define HOST_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Register snapshot. Layout of gpr[] depends on the host CPU:
 *   aarch64: gpr[0..30] = x0..x30 (x29 = fp, x30 = lr), ngpr = 31
 *   x86-64 : gpr[0..15] = rax rcx rdx rbx rsp rbp rsi rdi r8..r15 (Windows CONTEXT order), ngpr = 16
 * pc/sp are always filled. Writes to pc/sp/gpr are applied by set_context / a handled fault. */
typedef struct host_cpu_context {
    uint64_t pc;
    uint64_t sp;
    uint64_t gpr[31];
    uint64_t flags; /* nzcv / rflags */
    int ngpr;
} host_cpu_context;

#ifdef _WIN32
typedef struct { void *p; } host_mutex;     /* SRWLOCK (non-recursive) */
typedef struct { void *p; } host_cond;      /* CONDITION_VARIABLE */
typedef struct { void *p; } host_once_flag; /* INIT_ONCE */
typedef uint32_t host_tls_key;              /* TlsAlloc index */
#define HOST_MUTEX_INIT {0}
#define HOST_COND_INIT  {0}
#define HOST_ONCE_INIT  {0}
#else
typedef pthread_mutex_t host_mutex;
typedef pthread_cond_t host_cond;
typedef pthread_once_t host_once_flag;
typedef pthread_key_t host_tls_key;
#define HOST_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define HOST_COND_INIT  PTHREAD_COND_INITIALIZER
#define HOST_ONCE_INIT  PTHREAD_ONCE_INIT
#endif

#define HOST_TIMEDOUT 1

typedef struct host_thread host_thread;
typedef void *(*host_thread_fn)(void *arg);

/* stack_size 0 = default. Returns 0 or -1. */
int host_thread_create(host_thread **out, host_thread_fn fn, void *arg, size_t stack_size);
int host_thread_join(host_thread *t, void **ret); /* frees t */
host_thread *host_thread_self_open(void);        /* handle to the calling thread; free with host_thread_close */
void host_thread_close(host_thread *t);           /* release a handle without joining (detach) */
uint64_t host_thread_id(void);                    /* OS thread id of the caller */
uint64_t host_thread_get_id(host_thread *t);
void host_thread_yield(void);

void host_thread_set_name(const char *utf8_name); /* calling thread */
enum { HOST_PRIO_LOW = -1, HOST_PRIO_NORMAL = 0, HOST_PRIO_HIGH = 1, HOST_PRIO_REALTIME = 2 };
int host_thread_set_priority(int prio);           /* calling thread */

/* Diagnostics / preemption. Never suspend the calling thread. */
int host_thread_suspend(host_thread *t);
int host_thread_resume(host_thread *t);
int host_thread_get_context(host_thread *t, host_cpu_context *ctx); /* thread must be suspended */
int host_thread_set_context(host_thread *t, const host_cpu_context *ctx);

void host_mutex_init(host_mutex *m);
void host_mutex_destroy(host_mutex *m);
void host_mutex_lock(host_mutex *m);
int host_mutex_trylock(host_mutex *m); /* 0 = acquired */
void host_mutex_unlock(host_mutex *m);

void host_cond_init(host_cond *c);
void host_cond_destroy(host_cond *c);
void host_cond_wait(host_cond *c, host_mutex *m);
int host_cond_timedwait_ns(host_cond *c, host_mutex *m, uint64_t rel_ns); /* 0 or HOST_TIMEDOUT */
void host_cond_signal(host_cond *c);
void host_cond_broadcast(host_cond *c);

void host_once(host_once_flag *o, void (*fn)(void));

int host_tls_alloc(host_tls_key *key);
void host_tls_free(host_tls_key key);
void *host_tls_get(host_tls_key key);
int host_tls_set(host_tls_key key, void *value);

#ifdef __cplusplus
}
#endif
#endif
