/* Registration sections on Windows (gfx/host/dm_section.h): entries go to "<name>$m"; the linker sorts the "$" groups,
   so the markers below bracket them. getsectiondata returns the entries without the padding the linker may insert. */
#include "compat/mach-o/getsect.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const void *a, *b, *c; } Entry3;   /* hle_force_ent / GfxEntry: name, fn, why */
typedef struct { const void *a, *b; } Entry2;       /* nvn_impl: name, fn */
#define MARKERS(name, T)                                                                           \
    __attribute__((used, section(#name "$a"))) static const T name##_start = {0};                 \
    __attribute__((used, section(#name "$z"))) static const T name##_end = {0};
MARKERS(swnvn, Entry2)
MARKERS(totknvnx, Entry2)
MARKERS(totkgfx, Entry3)

static const void *collect(const uint8_t *lo, const uint8_t *hi, size_t entry, unsigned long *size) {
    size_t cap = (size_t)(hi - lo) / entry, n = 0;
    uint8_t *out = malloc(cap ? cap * entry : 1);
    if (!out) { *size = 0; return NULL; }
    for (const uint8_t *p = lo; p + entry <= hi; p += sizeof(void *)) {   /* entries start on pointer boundaries */
        const void *name; memcpy(&name, p, sizeof name);
        if (!name) continue;
        memcpy(out + n * entry, p, entry); n++; p += entry - sizeof(void *);
    }
    *size = (unsigned long)(n * entry);
    return out;
}
const void *getsectiondata(const void *header, const char *segment, const char *section, unsigned long *size) {
    (void)header; (void)segment;
    static const void *cache[3]; static unsigned long sizes[3];
    int i = !strcmp(section, "swnvn") ? 0 : !strcmp(section, "totknvnx") ? 1 : !strcmp(section, "totkgfx") ? 2 : -1;
    if (i < 0) { *size = 0; return NULL; }
    if (!cache[i]) {
        if (i == 0) cache[0] = collect((const uint8_t *)(&swnvn_start + 1), (const uint8_t *)&swnvn_end, sizeof(Entry2), &sizes[0]);
        if (i == 1) cache[1] = collect((const uint8_t *)(&totknvnx_start + 1), (const uint8_t *)&totknvnx_end, sizeof(Entry2), &sizes[1]);
        if (i == 2) cache[2] = collect((const uint8_t *)(&totkgfx_start + 1), (const uint8_t *)&totkgfx_end, sizeof(Entry3), &sizes[2]);
    }
    *size = sizes[i];
    return cache[i];
}
