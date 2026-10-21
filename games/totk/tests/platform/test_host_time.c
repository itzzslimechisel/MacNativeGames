/* Monotonic clocks and sleep. */
#include "host_time.h"
#include "test_common.h"

int main(void) {
    CHECK(host_counter_freq() > 0);
    uint64_t prev = host_now_ns(), prevc = host_monotonic_continuous_ns(), prevt = host_counter();
    int mono = 1;
    for (int i = 0; i < 200000; i++) {
        uint64_t n = host_now_ns(), c = host_monotonic_continuous_ns(), t = host_counter();
        mono &= n >= prev && c >= prevc && t >= prevt;
        prev = n; prevc = c; prevt = t;
    }
    CHECK(mono);

    uint64_t t0 = host_now_ns(), c0 = host_monotonic_continuous_ns(), k0 = host_counter();
    host_sleep_ns(20000000ull);
    uint64_t dt = host_now_ns() - t0, dc = host_monotonic_continuous_ns() - c0;
    uint64_t dk = (host_counter() - k0) * 1000000000ull / host_counter_freq();
    CHECK(dt >= 19000000ull && dt < 1000000000ull);
    CHECK(dc >= 18000000ull && dc < 1000000000ull);
    CHECK(dk >= 19000000ull && dk < 1000000000ull);

    /* Short sleeps return promptly. */
    t0 = host_now_ns();
    for (int i = 0; i < 10; i++) host_sleep_ns(100000ull);
    CHECK(host_now_ns() - t0 < 500000000ull);
    TEST_DONE("test_host_time");
}
