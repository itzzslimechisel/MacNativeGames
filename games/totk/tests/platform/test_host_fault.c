/* Guarded reads of unmapped memory and write-fault handling (nvn_dirty-style). */
#include "host_fault.h"
#include "host_mem.h"
#include "test_common.h"
#include <string.h>

static char *g_page;
static size_t g_pg;
static int g_hits;
static host_access g_access;

static int dirty_handler(host_fault_info *fi) {
    if (fi->kind != HOST_FAULT_ACCESS) return HOST_FAULT_CONTINUE;
    if (fi->addr < (uint64_t)(uintptr_t)g_page || fi->addr >= (uint64_t)(uintptr_t)g_page + g_pg) return HOST_FAULT_CONTINUE;
    g_hits++;
    g_access = fi->access;
    CHECK(fi->ctx && fi->ctx->pc != 0 && fi->ctx->sp != 0);
    host_mem_protect(g_page, g_pg, HOST_PROT_RW); /* mark dirty, let the write proceed */
    return HOST_FAULT_HANDLED;
}

static int other_handler(host_fault_info *fi) { (void)fi; return HOST_FAULT_CONTINUE; }

int main(void) {
    size_t g = host_alloc_granularity(), pg = host_page_size();
    char *r = (char *)host_mem_reserve(NULL, 4 * g, 0);
    REQUIRE(r != NULL);
    CHECK(host_mem_commit(r, pg, HOST_PROT_RW) == 0);
    memset(r, 0x5A, pg);

    char buf[256];
    /* Fully readable. */
    CHECK(host_guarded_read(buf, r, 64) == 64 && buf[63] == 0x5A);
    /* Fully unmapped (reserved only) and NULL page. */
    CHECK(host_guarded_read(buf, r + pg, 64) == 0);
    CHECK(host_guarded_read(buf, (void *)16, 8) == 0);
    /* Straddling committed -> uncommitted: exact partial count. */
    memset(buf, 0, sizeof buf);
    CHECK(host_guarded_read(buf, r + pg - 40, 200) == 40);
    CHECK(buf[0] == 0x5A && buf[39] == 0x5A);
    /* Many guarded faults in a row stay stable. */
    int ok = 1;
    for (int i = 0; i < 1000; i++) ok &= host_guarded_read(buf, r + pg + (size_t)i, 4) == 0;
    CHECK(ok);
    /* Released memory. */
    char *gone = (char *)host_mem_reserve(NULL, g, 0);
    REQUIRE(gone);
    host_mem_release(gone, g);
    CHECK(host_guarded_read(buf, gone, 8) == 0);

    /* Write fault on a read-only page, resolved by a handler (dirty tracking pattern). */
    CHECK(host_fault_add_handler(other_handler) == 0);
    CHECK(host_fault_add_handler(dirty_handler) == 0);
    g_page = r; g_pg = pg;
    CHECK(host_mem_protect(r, pg, HOST_PROT_R) == 0);
    volatile char *vp = r + 123;
    *vp = 0x77;
    CHECK(g_hits == 1);
    CHECK(g_access == HOST_ACCESS_WRITE);
    CHECK(r[123] == 0x77);
    *vp = 0x78; /* now RW: no fault */
    CHECK(g_hits == 1);
    /* Guarded read on a read fault does not reach handlers. */
    CHECK(host_guarded_read(buf, r + pg, 4) == 0);
    CHECK(g_hits == 1);
    CHECK(host_fault_remove_handler(dirty_handler) == 0);
    CHECK(host_fault_remove_handler(dirty_handler) == -1);
    CHECK(host_fault_remove_handler(other_handler) == 0);

    host_mem_release(r, 4 * g);
    TEST_DONE("test_host_fault");
}
