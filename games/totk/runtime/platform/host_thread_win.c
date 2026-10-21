/* host_thread_win.c - host_thread.h on Windows: CreateThread, SRWLOCK, CONDITION_VARIABLE, INIT_ONCE, TLS. */
#include "host_thread.h"
#include "host_win_internal.h"
#include <stdlib.h>

_Static_assert(sizeof(host_mutex) == sizeof(SRWLOCK), "host_mutex must hold an SRWLOCK");
_Static_assert(sizeof(host_cond) == sizeof(CONDITION_VARIABLE), "host_cond must hold a CONDITION_VARIABLE");
_Static_assert(sizeof(host_once_flag) == sizeof(INIT_ONCE), "host_once_flag must hold an INIT_ONCE");

struct host_thread {
    HANDLE h;
    DWORD id;
    volatile LONG refs; /* creator handle + running thread */
    host_thread_fn fn;
    void *arg;
    void *ret;
};

static void thread_unref(host_thread *t) {
    if (InterlockedDecrement(&t->refs) == 0) {
        CloseHandle(t->h);
        free(t);
    }
}

static DWORD WINAPI thread_start(LPVOID p) {
    host_thread *t = (host_thread *)p;
    t->ret = t->fn(t->arg);
    thread_unref(t);
    return 0;
}

int host_thread_create(host_thread **out, host_thread_fn fn, void *arg, size_t stack_size) {
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    if (!t) return -1;
    t->fn = fn; t->arg = arg; t->refs = 2;
    t->h = CreateThread(NULL, stack_size, thread_start, t, stack_size ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, &t->id);
    if (!t->h) { free(t); return -1; }
    *out = t;
    return 0;
}

int host_thread_join(host_thread *t, void **ret) {
    if (!t || WaitForSingleObject(t->h, INFINITE) != WAIT_OBJECT_0) return -1;
    if (ret) *ret = t->ret;
    thread_unref(t);
    return 0;
}

host_thread *host_thread_self_open(void) {
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        free(t);
        return NULL;
    }
    t->id = GetCurrentThreadId();
    t->refs = 1;
    return t;
}

void host_thread_close(host_thread *t) { if (t) thread_unref(t); }
uint64_t host_thread_id(void) { return GetCurrentThreadId(); }
uint64_t host_thread_get_id(host_thread *t) { return t ? t->id : 0; }
void host_thread_yield(void) { SwitchToThread(); }

typedef HRESULT(WINAPI *pfn_SetThreadDescription)(HANDLE, PCWSTR);

void host_thread_set_name(const char *utf8_name) {
    static pfn_SetThreadDescription fn;
    static volatile LONG resolved;
    if (!resolved) { fn = (pfn_SetThreadDescription)(void *)host_win_proc("SetThreadDescription"); InterlockedExchange(&resolved, 1); }
    if (!fn || !utf8_name) return;
    wchar_t *w = host_win_widen(utf8_name, 0);
    if (w) { fn(GetCurrentThread(), w); free(w); }
}

int host_thread_set_priority(int prio) {
    int p = THREAD_PRIORITY_NORMAL;
    if (prio <= HOST_PRIO_LOW) p = THREAD_PRIORITY_BELOW_NORMAL;
    else if (prio == HOST_PRIO_HIGH) p = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (prio >= HOST_PRIO_REALTIME) p = THREAD_PRIORITY_HIGHEST;
    return SetThreadPriority(GetCurrentThread(), p) ? 0 : -1;
}

int host_thread_suspend(host_thread *t) { return SuspendThread(t->h) == (DWORD)-1 ? -1 : 0; }
int host_thread_resume(host_thread *t) { return ResumeThread(t->h) == (DWORD)-1 ? -1 : 0; }

int host_thread_get_context(host_thread *t, host_cpu_context *ctx) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(t->h, &c)) return -1;
    host_ctx_from_native(&c, ctx);
    return 0;
}

int host_thread_set_context(host_thread *t, const host_cpu_context *ctx) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(t->h, &c)) return -1;
    host_ctx_to_native(ctx, &c);
    return SetThreadContext(t->h, &c) ? 0 : -1;
}

void host_mutex_init(host_mutex *m) { InitializeSRWLock((PSRWLOCK)m); }
void host_mutex_destroy(host_mutex *m) { (void)m; }
void host_mutex_lock(host_mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
int host_mutex_trylock(host_mutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) ? 0 : -1; }
void host_mutex_unlock(host_mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void host_cond_init(host_cond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void host_cond_destroy(host_cond *c) { (void)c; }
void host_cond_wait(host_cond *c, host_mutex *m) {
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0);
}
int host_cond_timedwait_ns(host_cond *c, host_mutex *m, uint64_t rel_ns) {
    uint64_t ms = rel_ns / 1000000u + (rel_ns % 1000000u ? 1 : 0);
    if (ms >= INFINITE) ms = INFINITE - 1;
    if (SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, (DWORD)ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? HOST_TIMEDOUT : 0;
}
void host_cond_signal(host_cond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void host_cond_broadcast(host_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

static BOOL CALLBACK once_tramp(PINIT_ONCE o, PVOID param, PVOID *ctx) {
    (void)o; (void)ctx;
    void (*fn)(void);
    memcpy(&fn, &param, sizeof fn);
    fn();
    return TRUE;
}
void host_once(host_once_flag *o, void (*fn)(void)) {
    PVOID param;
    memcpy(&param, &fn, sizeof param);
    InitOnceExecuteOnce((PINIT_ONCE)o, once_tramp, param, NULL);
}

int host_tls_alloc(host_tls_key *key) {
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) return -1;
    *key = k;
    return 0;
}
void host_tls_free(host_tls_key key) { TlsFree(key); }
void *host_tls_get(host_tls_key key) { return TlsGetValue(key); }
int host_tls_set(host_tls_key key, void *value) { return TlsSetValue(key, value) ? 0 : -1; }
