/* Threads, mutex, condition variables, once, TLS, suspend/context. */
#include "host_thread.h"
#include "host_time.h"
#include "test_common.h"
#include <string.h>

static host_mutex g_m = HOST_MUTEX_INIT;
static host_cond g_c = HOST_COND_INIT;
static long g_counter;

static void *adder(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) { host_mutex_lock(&g_m); g_counter++; host_mutex_unlock(&g_m); }
    return (void *)(uintptr_t)host_thread_id();
}

/* Bounded queue producer/consumer. */
static int q[8], qh, qt, qn;
static host_cond g_not_full = HOST_COND_INIT, g_not_empty = HOST_COND_INIT;
static void *producer(void *arg) {
    (void)arg;
    for (int i = 1; i <= 1000; i++) {
        host_mutex_lock(&g_m);
        while (qn == 8) host_cond_wait(&g_not_full, &g_m);
        q[qt] = i; qt = (qt + 1) % 8; qn++;
        host_cond_signal(&g_not_empty);
        host_mutex_unlock(&g_m);
    }
    return NULL;
}

static host_once_flag g_once = HOST_ONCE_INIT;
static volatile long g_once_calls;
static void once_fn(void) { g_once_calls++; }

static host_tls_key g_key;
static void *tls_worker(void *arg) {
    host_tls_set(g_key, arg);
    host_once(&g_once, once_fn);
    host_thread_set_name("tls_worker");
    host_thread_yield();
    return host_tls_get(g_key);
}

static volatile int g_spin_stop;
static void *spinner(void *arg) {
    (void)arg;
    while (!g_spin_stop) {}
    return NULL;
}

int main(void) {
    /* Create/join + mutex contention. */
    host_thread *t[4];
    for (int i = 0; i < 4; i++) REQUIRE(host_thread_create(&t[i], adder, NULL, 0) == 0);
    uint64_t ids[4];
    for (int i = 0; i < 4; i++) {
        uint64_t expect = host_thread_get_id(t[i]);
        void *ret;
        CHECK(host_thread_join(t[i], &ret) == 0);
        ids[i] = (uint64_t)(uintptr_t)ret;
        CHECK(ids[i] == expect && ids[i] != host_thread_id());
    }
    CHECK(g_counter == 400000);
    CHECK(host_mutex_trylock(&g_m) == 0);
    host_mutex_unlock(&g_m);

    /* Condition variables. */
    host_thread *pt;
    REQUIRE(host_thread_create(&pt, producer, NULL, 64 * 1024) == 0);
    long sum = 0;
    for (int i = 0; i < 1000; i++) {
        host_mutex_lock(&g_m);
        while (qn == 0) host_cond_wait(&g_not_empty, &g_m);
        sum += q[qh]; qh = (qh + 1) % 8; qn--;
        host_cond_signal(&g_not_full);
        host_mutex_unlock(&g_m);
    }
    CHECK(host_thread_join(pt, NULL) == 0);
    CHECK(sum == 1000L * 1001L / 2);

    /* Timed wait times out. */
    host_mutex_lock(&g_m);
    uint64_t t0 = host_now_ns();
    int rc = host_cond_timedwait_ns(&g_c, &g_m, 20000000ull);
    uint64_t dt = host_now_ns() - t0;
    host_mutex_unlock(&g_m);
    CHECK(rc == HOST_TIMEDOUT);
    CHECK(dt >= 15000000ull && dt < 2000000000ull);

    /* Once + TLS. */
    REQUIRE(host_tls_alloc(&g_key) == 0);
    host_tls_set(g_key, (void *)0x1234);
    host_thread *w[3];
    for (int i = 0; i < 3; i++) REQUIRE(host_thread_create(&w[i], tls_worker, (void *)(uintptr_t)(i + 1), 0) == 0);
    for (int i = 0; i < 3; i++) {
        void *ret;
        CHECK(host_thread_join(w[i], &ret) == 0);
        CHECK(ret == (void *)(uintptr_t)(i + 1));
    }
    CHECK(host_tls_get(g_key) == (void *)0x1234);
    CHECK(g_once_calls == 1);
    host_tls_free(g_key);

    /* Suspend / get_context / resume. */
    host_thread *sp;
    REQUIRE(host_thread_create(&sp, spinner, NULL, 0) == 0);
    host_sleep_ns(10000000ull);
    CHECK(host_thread_suspend(sp) == 0);
    host_cpu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    CHECK(host_thread_get_context(sp, &ctx) == 0);
    CHECK(ctx.pc != 0 && ctx.sp != 0 && ctx.ngpr > 0);
    CHECK(host_thread_set_context(sp, &ctx) == 0);
    CHECK(host_thread_resume(sp) == 0);
    g_spin_stop = 1;
    CHECK(host_thread_join(sp, NULL) == 0);

    /* Self handle + priority. */
    host_thread *self = host_thread_self_open();
    CHECK(self && host_thread_get_id(self) == host_thread_id());
    host_thread_close(self);
    CHECK(host_thread_set_priority(HOST_PRIO_HIGH) == 0);
    CHECK(host_thread_set_priority(HOST_PRIO_NORMAL) == 0);

    TEST_DONE("test_host_thread");
}
