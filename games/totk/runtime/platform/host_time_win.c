/* host_time_win.c - host_time.h on Windows: QueryPerformanceCounter, QueryInterruptTimePrecise,
 * high-resolution waitable timers for sleeps. */
#include "host_time.h"
#include "host_win_internal.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef VOID(WINAPI *pfn_QueryInterruptTimePrecise)(PULONGLONG);

static uint64_t g_freq;
static pfn_QueryInterruptTimePrecise p_qitp;
static volatile LONG g_init;

static void init(void) {
    if (g_init) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (uint64_t)f.QuadPart;
    p_qitp = (pfn_QueryInterruptTimePrecise)(void *)host_win_proc("QueryInterruptTimePrecise");
    InterlockedExchange(&g_init, 1);
}

static uint64_t ticks_to_ns(uint64_t t, uint64_t f) {
    return (t / f) * 1000000000ull + (t % f) * 1000000000ull / f;
}

uint64_t host_counter(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
uint64_t host_counter_freq(void) { init(); return g_freq; }
uint64_t host_now_ns(void) { init(); return ticks_to_ns(host_counter(), g_freq); }

uint64_t host_monotonic_continuous_ns(void) {
    init();
    if (p_qitp) { ULONGLONG t; p_qitp(&t); return (uint64_t)t * 100u; } /* 100 ns units, includes sleep */
    return host_now_ns(); /* QPC also advances across sleep on current Windows */
}

void host_sleep_ns(uint64_t ns) {
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer && ns >= 1000) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ns / 100u); /* relative, 100 ns units */
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    if (ns == 0) { SwitchToThread(); return; }
    Sleep((DWORD)((ns + 999999u) / 1000000u));
}
