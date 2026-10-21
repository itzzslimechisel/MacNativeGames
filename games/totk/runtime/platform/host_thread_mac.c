/* host_thread_mac.c - host_thread.h on macOS/iOS: pthreads, QoS classes, Mach thread suspend/state
 * (the same calls runtime/threads.c uses today). Register access is implemented for arm64 only. */
#include "host_thread.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

struct host_thread { pthread_t t; int owned; };

int host_thread_create(host_thread **out, host_thread_fn fn, void *arg, size_t stack_size) {
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    if (!t) return -1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack_size) pthread_attr_setstacksize(&a, stack_size);
    int rc = pthread_create(&t->t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc) { free(t); return -1; }
    t->owned = 1;
    *out = t;
    return 0;
}

int host_thread_join(host_thread *t, void **ret) {
    if (!t || pthread_join(t->t, ret)) return -1;
    free(t);
    return 0;
}

host_thread *host_thread_self_open(void) {
    host_thread *t = (host_thread *)calloc(1, sizeof *t);
    if (t) t->t = pthread_self();
    return t;
}

void host_thread_close(host_thread *t) {
    if (!t) return;
    if (t->owned) pthread_detach(t->t);
    free(t);
}

uint64_t host_thread_id(void) { uint64_t id = 0; pthread_threadid_np(NULL, &id); return id; }
uint64_t host_thread_get_id(host_thread *t) { uint64_t id = 0; if (t) pthread_threadid_np(t->t, &id); return id; }
void host_thread_yield(void) { sched_yield(); }
void host_thread_set_name(const char *utf8_name) { if (utf8_name) pthread_setname_np(utf8_name); }

int host_thread_set_priority(int prio) {
    qos_class_t q = QOS_CLASS_USER_INITIATED;
    if (prio <= HOST_PRIO_LOW) q = QOS_CLASS_UTILITY;
    else if (prio >= HOST_PRIO_HIGH) q = QOS_CLASS_USER_INTERACTIVE;
    return pthread_set_qos_class_self_np(q, 0) == 0 ? 0 : -1;
}

int host_thread_suspend(host_thread *t) { return thread_suspend(pthread_mach_thread_np(t->t)) == KERN_SUCCESS ? 0 : -1; }
int host_thread_resume(host_thread *t) { return thread_resume(pthread_mach_thread_np(t->t)) == KERN_SUCCESS ? 0 : -1; }

int host_thread_get_context(host_thread *t, host_cpu_context *ctx) {
#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(pthread_mach_thread_np(t->t), ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS)
        return -1;
    memset(ctx, 0, sizeof *ctx);
    for (int i = 0; i < 29; i++) ctx->gpr[i] = st.__x[i];
    ctx->gpr[29] = (uint64_t)arm_thread_state64_get_fp(st);
    ctx->gpr[30] = (uint64_t)arm_thread_state64_get_lr(st);
    ctx->sp = (uint64_t)arm_thread_state64_get_sp(st);
    ctx->pc = (uint64_t)arm_thread_state64_get_pc(st);
    ctx->flags = st.__cpsr;
    ctx->ngpr = 31;
    return 0;
#else
    (void)t; (void)ctx;
    return -1;
#endif
}

int host_thread_set_context(host_thread *t, const host_cpu_context *ctx) {
#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    thread_act_t port = pthread_mach_thread_np(t->t);
    if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) return -1;
    for (int i = 0; i < 29; i++) st.__x[i] = ctx->gpr[i];
    arm_thread_state64_set_fp(st, ctx->gpr[29]);
    arm_thread_state64_set_lr_fptr(st, (void *)(uintptr_t)ctx->gpr[30]);
    arm_thread_state64_set_sp(st, ctx->sp);
    arm_thread_state64_set_pc_fptr(st, (void *)(uintptr_t)ctx->pc);
    st.__cpsr = (uint32_t)ctx->flags;
    return thread_set_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, n) == KERN_SUCCESS ? 0 : -1;
#else
    (void)t; (void)ctx;
    return -1;
#endif
}

void host_mutex_init(host_mutex *m) { pthread_mutex_init(m, NULL); }
void host_mutex_destroy(host_mutex *m) { pthread_mutex_destroy(m); }
void host_mutex_lock(host_mutex *m) { pthread_mutex_lock(m); }
int host_mutex_trylock(host_mutex *m) { return pthread_mutex_trylock(m) == 0 ? 0 : -1; }
void host_mutex_unlock(host_mutex *m) { pthread_mutex_unlock(m); }

void host_cond_init(host_cond *c) { pthread_cond_init(c, NULL); }
void host_cond_destroy(host_cond *c) { pthread_cond_destroy(c); }
void host_cond_wait(host_cond *c, host_mutex *m) { pthread_cond_wait(c, m); }
int host_cond_timedwait_ns(host_cond *c, host_mutex *m, uint64_t rel_ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(rel_ns / 1000000000u);
    ts.tv_nsec = (long)(rel_ns % 1000000000u);
    return pthread_cond_timedwait_relative_np(c, m, &ts) == ETIMEDOUT ? HOST_TIMEDOUT : 0;
}
void host_cond_signal(host_cond *c) { pthread_cond_signal(c); }
void host_cond_broadcast(host_cond *c) { pthread_cond_broadcast(c); }

void host_once(host_once_flag *o, void (*fn)(void)) { pthread_once(o, fn); }

int host_tls_alloc(host_tls_key *key) { return pthread_key_create(key, NULL) == 0 ? 0 : -1; }
void host_tls_free(host_tls_key key) { pthread_key_delete(key); }
void *host_tls_get(host_tls_key key) { return pthread_getspecific(key); }
int host_tls_set(host_tls_key key, void *value) { return pthread_setspecific(key, value) == 0 ? 0 : -1; }
