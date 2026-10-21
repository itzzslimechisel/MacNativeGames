/* Fixed reservations below 2^39, commit/decommit/protect, section aliasing. */
#include "host_fault.h"
#include "host_mem.h"
#include "test_common.h"
#include <stdint.h>
#include <string.h>

#define LIMIT39 (1ull << 39)
#define MiB (1024ull * 1024ull)
#define GiB (1024ull * MiB)

static void test_fixed(void) {
    size_t g = host_alloc_granularity(), pg = host_page_size();
    CHECK(g >= pg && (g % pg) == 0);
#ifdef _WIN32
    CHECK(g == 64 * 1024);
#endif
    /* A fixed address in the low guest window (64 GiB), as the runtime's layout does. */
    void *want = (void *)(uintptr_t)(64ull * GiB);
    void *r = host_mem_reserve(want, 256 * MiB, HOST_RESERVE_FIXED);
    REQUIRE(r == want);
    CHECK((uint64_t)(uintptr_t)r + 256 * MiB <= LIMIT39);
    /* Fixed reservation over an occupied range must fail, never overwrite. */
    CHECK(host_mem_reserve((char *)want + g, g, HOST_RESERVE_FIXED) == NULL);

    /* Reserved but uncommitted: unreadable. */
    unsigned char probe[16];
    CHECK(host_guarded_read(probe, r, sizeof probe) == 0);

    CHECK(host_mem_commit(r, 4 * pg, HOST_PROT_RW) == 0);
    unsigned char *p = (unsigned char *)r;
    CHECK(p[0] == 0 && p[4 * pg - 1] == 0);
    memset(p, 0xAB, 4 * pg);
    CHECK(host_mem_protect(r, 4 * pg, HOST_PROT_R) == 0);
    CHECK(host_guarded_read(probe, p + pg, sizeof probe) == sizeof probe && probe[3] == 0xAB);
    CHECK(host_mem_decommit(r, 4 * pg) == 0);
    CHECK(host_guarded_read(probe, r, sizeof probe) == 0);
    /* Recommit gives zeroed memory. */
    CHECK(host_mem_commit(r, pg, HOST_PROT_RW) == 0);
    CHECK(p[0] == 0 && p[pg - 1] == 0);
    CHECK(host_mem_release(r, 256 * MiB) == 0);
    CHECK(host_guarded_read(probe, r, 1) == 0);

    /* Large alias-region sized reservation (64 GiB) placed by hole search below 2^39. */
    void *big = host_mem_reserve_in_range(4 * GiB, LIMIT39, 64 * GiB, 0);
    CHECK(big != NULL);
    if (big) {
        CHECK((uint64_t)(uintptr_t)big >= 4 * GiB && (uint64_t)(uintptr_t)big + 64 * GiB <= LIMIT39);
        CHECK(host_mem_commit((char *)big + 63 * GiB, pg, HOST_PROT_RW) == 0);
        ((volatile char *)big)[63 * GiB] = 1;
        CHECK(host_mem_release(big, 64 * GiB) == 0);
    }
}

static void test_alias(void) {
    size_t g = host_alloc_granularity();
    size_t ssz = 4 * g;
    host_section *s = host_section_create(ssz);
    REQUIRE(s != NULL);

    /* Placeholder reservation below 2^39 receiving two views of the same section pages. */
    size_t rsz = 16 * g;
    void *base = host_mem_reserve_in_range(128ull * GiB, LIMIT39, rsz, HOST_RESERVE_PLACEHOLDER);
    REQUIRE(base != NULL);
    CHECK((uint64_t)(uintptr_t)base + rsz <= LIMIT39);
    char *a = (char *)host_section_map(s, g, 2 * g, base, HOST_PROT_RW);
    char *b = (char *)host_section_map(s, g, 2 * g, (char *)base + 8 * g, HOST_PROT_RW);
    REQUIRE(a == (char *)base);
    REQUIRE(b == (char *)base + 8 * g);
    a[0] = 0x11; a[2 * g - 1] = 0x22;
    CHECK(b[0] == 0x11 && b[2 * g - 1] == 0x22);
    b[100] = 0x33;
    CHECK(a[100] == 0x33);

    /* A third view at an OS-chosen address, offset 0: page 1 of it is page 0 of a/b. */
    char *c = (char *)host_section_map(s, 0, ssz, NULL, HOST_PROT_R);
    REQUIRE(c != NULL);
    CHECK(c[g] == 0x11 && c[g + 100] == 0x33);
    CHECK(host_section_unmap(c, ssz, 0) == 0);

    /* Unmap one view back to a placeholder, map again at the same place. */
    CHECK(host_section_unmap(b, 2 * g, 1) == 0);
    unsigned char probe;
    CHECK(host_guarded_read(&probe, b, 1) == 0);
    b = (char *)host_section_map(s, g, 2 * g, (char *)base + 8 * g, HOST_PROT_R);
    CHECK(b == (char *)base + 8 * g);
    if (b) CHECK(b[0] == 0x11);

    CHECK(host_section_unmap(a, 2 * g, 1) == 0);
    if (b) CHECK(host_section_unmap(b, 2 * g, 1) == 0);
    CHECK(host_mem_release(base, rsz) == 0);
    host_section_destroy(s);
}

int main(void) {
    test_fixed();
    test_alias();
    TEST_DONE("test_host_mem");
}
