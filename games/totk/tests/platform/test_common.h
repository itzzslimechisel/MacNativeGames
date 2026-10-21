/* test_common.h - minimal test harness (no framework). */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
    } while (0)
#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) { fprintf(stderr, "FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
    } while (0)
#define TEST_DONE(name)                                                                  \
    do {                                                                                 \
        if (g_fail) { fprintf(stderr, "%s: %d failure(s)\n", name, g_fail); return 1; }  \
        printf("%s: ok\n", name); return 0;                                              \
    } while (0)
#endif
