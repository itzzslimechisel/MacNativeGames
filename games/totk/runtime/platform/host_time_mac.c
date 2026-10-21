/* host_time_mac.c - host_time.h on macOS/iOS: mach_absolute_time / mach_continuous_time + timebase
 * (same math as runtime_now_ns in runtime/loader.c and time_host.c). */
#include "host_time.h"
#include <mach/mach_time.h>
#include <time.h>

static mach_timebase_info_data_t tb(void) {
    static mach_timebase_info_data_t t;
    if (!t.denom) mach_timebase_info(&t); /* idempotent; benign race */
    return t;
}

static uint64_t to_ns(uint64_t ticks) {
    mach_timebase_info_data_t t = tb();
    return (uint64_t)((__uint128_t)ticks * t.numer / t.denom);
}

uint64_t host_now_ns(void) { return to_ns(mach_absolute_time()); }
uint64_t host_monotonic_continuous_ns(void) { return to_ns(mach_continuous_time()); }
uint64_t host_counter(void) { return mach_absolute_time(); }
uint64_t host_counter_freq(void) {
    mach_timebase_info_data_t t = tb();
    return (uint64_t)((__uint128_t)1000000000u * t.denom / t.numer);
}

void host_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    while (nanosleep(&ts, &ts) != 0) {}
}
