/* host_mem_mac.c - host_mem.h on macOS/iOS: mmap/mprotect, mach_vm_allocate(FIXED), mach_vm_remap aliasing
 * (same primitives as runtime/services.c and gfx/nvn/totk_vpool.c). */
#include "host_mem.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static int posix_prot(int prot) {
    int p = PROT_NONE;
    if (prot & HOST_PROT_R) p |= PROT_READ;
    if (prot & HOST_PROT_W) p |= PROT_WRITE;
    if (prot & HOST_PROT_X) p |= PROT_EXEC;
    return p;
}

size_t host_page_size(void) { return (size_t)getpagesize(); }
size_t host_alloc_granularity(void) { return (size_t)getpagesize(); }

void *host_mem_reserve(void *addr, size_t size, int flags) {
    if (addr && (flags & HOST_RESERVE_FIXED)) {
        mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)addr;
        if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) return NULL;
        if (mprotect(addr, size, PROT_NONE) != 0) { mach_vm_deallocate(mach_task_self(), a, size); return NULL; }
        return addr;
    }
    void *r = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return r == MAP_FAILED ? NULL : r;
}

void *host_mem_reserve_in_range(uint64_t lo, uint64_t hi, size_t size, int flags) {
    (void)flags;
    mach_vm_address_t a = lo; /* VM_FLAGS_ANYWHERE searches upward from the hint */
    if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return NULL;
    if (a < lo || a + size > hi) { mach_vm_deallocate(mach_task_self(), a, size); return NULL; }
    if (mprotect((void *)(uintptr_t)a, size, PROT_NONE) != 0) { mach_vm_deallocate(mach_task_self(), a, size); return NULL; }
    return (void *)(uintptr_t)a;
}

int host_mem_release(void *addr, size_t size) { return munmap(addr, size) == 0 ? 0 : -1; }

int host_mem_commit(void *addr, size_t size, int prot) {
    return mprotect(addr, size, posix_prot(prot)) == 0 ? 0 : -1; /* anonymous memory is zero-fill on demand */
}

int host_mem_decommit(void *addr, size_t size) {
    void *r = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return r == addr ? 0 : -1;
}

int host_mem_protect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, posix_prot(prot)) == 0 ? 0 : -1;
}

void host_flush_icache(void *addr, size_t size) { sys_icache_invalidate(addr, size); }

struct host_section { mach_vm_address_t base; size_t size; };

host_section *host_section_create(size_t size) {
    host_section *s = (host_section *)malloc(sizeof *s);
    if (!s) return NULL;
    s->base = 0;
    s->size = size;
    if (mach_vm_allocate(mach_task_self(), &s->base, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { free(s); return NULL; }
    return s;
}

void host_section_destroy(host_section *s) {
    if (!s) return;
    mach_vm_deallocate(mach_task_self(), s->base, s->size); /* remapped views keep the shared object alive */
    free(s);
}

void *host_section_map(host_section *s, uint64_t offset, size_t size, void *addr, int prot) {
    if (!s || offset + size > s->size) return NULL;
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)addr;
    vm_prot_t cur = 0, max = 0;
    int fl = addr ? (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) : VM_FLAGS_ANYWHERE;
    if (mach_vm_remap(mach_task_self(), &a, size, 0, fl, mach_task_self(), s->base + offset, FALSE, &cur, &max,
                      VM_INHERIT_NONE) != KERN_SUCCESS)
        return NULL;
    if (mprotect((void *)(uintptr_t)a, size, posix_prot(prot)) != 0) {
        mach_vm_deallocate(mach_task_self(), a, size);
        return NULL;
    }
    return (void *)(uintptr_t)a;
}

int host_section_unmap(void *view, size_t size, int keep_reserved) {
    if (keep_reserved) return host_mem_decommit(view, size);
    return munmap(view, size) == 0 ? 0 : -1;
}
