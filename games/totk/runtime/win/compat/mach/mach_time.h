/* Windows stand-in for mach_absolute_time (QueryPerformanceCounter ticks). */
#pragma once
#include <stdint.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
uint64_t mach_absolute_time(void);
uint64_t mach_continuous_time(void);
int mach_timebase_info(mach_timebase_info_data_t *info);
void mach_wait_until(uint64_t deadline);   /* QueryPerformanceCounter ticks */
