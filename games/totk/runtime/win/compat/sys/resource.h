/* Windows stand-in for getrusage (process CPU times from GetProcessTimes). */
#pragma once
#include <time.h>
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval { long tv_sec; long tv_usec; };
#endif
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss; };
#define RUSAGE_SELF 0
int getrusage(int who, struct rusage *ru);
