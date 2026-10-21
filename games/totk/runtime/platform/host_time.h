/* host_time.h - monotonic clocks (macOS: mach_absolute_time / mach_continuous_time,
 * Windows: QueryPerformanceCounter / QueryInterruptTimePrecise). */
#ifndef HOST_TIME_H
#define HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t host_now_ns(void);                  /* monotonic, high resolution (replaces runtime_now_ns) */
uint64_t host_monotonic_continuous_ns(void); /* monotonic, keeps counting across system sleep */
uint64_t host_counter(void);                 /* raw counter ticks for cheap deltas */
uint64_t host_counter_freq(void);            /* ticks per second of host_counter() */
void host_sleep_ns(uint64_t ns);

#ifdef __cplusplus
}
#endif
#endif
