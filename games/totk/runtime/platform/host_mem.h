/* host_mem.h - host virtual memory: fixed-address reservations below 2^39, commit/decommit/protect,
 * icache flush, and page aliasing through shareable sections (NVN virtual pools).
 *
 * Implementations: host_mem_mac.c (mmap/mprotect/mach_vm_remap), host_mem_win.c (VirtualAlloc2/MapViewOfFile3).
 * Portable: no assumption about the host CPU (x86-64 or ARM64) or page size. Callers must align addresses and
 * sizes to host_alloc_granularity() for reserve/section calls and to host_page_size() for commit/protect.
 * All functions return 0 on success and -1 on failure unless stated otherwise. */
#ifndef HOST_MEM_H
#define HOST_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOST_PROT_NONE = 0,
    HOST_PROT_R    = 1,
    HOST_PROT_W    = 2,
    HOST_PROT_X    = 4,
    HOST_PROT_RW   = HOST_PROT_R | HOST_PROT_W,
    HOST_PROT_RX   = HOST_PROT_R | HOST_PROT_X,
    HOST_PROT_RWX  = HOST_PROT_R | HOST_PROT_W | HOST_PROT_X,
};

/* Reservation flags. */
enum {
    HOST_RESERVE_FIXED       = 1, /* fail unless the range is reserved exactly at addr (never overwrite) */
    HOST_RESERVE_PLACEHOLDER = 2, /* range may later receive section views (host_section_map with addr) */
};

size_t host_page_size(void);        /* commit/protect granularity: 4 KiB on Windows, 16 KiB on Apple silicon */
size_t host_alloc_granularity(void); /* reserve/view granularity: 64 KiB on Windows, page size on macOS */

/* Reserve [addr, addr+size) inaccessible, uncommitted. addr == NULL lets the OS choose.
 * Returns the base or NULL. Without HOST_RESERVE_FIXED a different address may be returned. */
void *host_mem_reserve(void *addr, size_t size, int flags);
/* First free range of `size` bytes at or above `lo` whose end is <= `hi` (both granularity aligned); NULL if none. */
void *host_mem_reserve_in_range(uint64_t lo, uint64_t hi, size_t size, int flags);
/* Release a whole reservation made by host_mem_reserve* (addr/size as returned/requested). */
int host_mem_release(void *addr, size_t size);

int host_mem_commit(void *addr, size_t size, int prot);  /* back with zeroed memory and set protection */
int host_mem_decommit(void *addr, size_t size);         /* drop backing, range becomes inaccessible, stays reserved */
int host_mem_protect(void *addr, size_t size, int prot);
void host_flush_icache(void *addr, size_t size);

/* Shareable memory sections: the same physical pages mapped at several addresses (true aliasing). */
typedef struct host_section host_section;

host_section *host_section_create(size_t size); /* size: multiple of host_alloc_granularity(); contents zero */
void host_section_destroy(host_section *s);      /* views already mapped stay valid until unmapped */
/* Map [offset, offset+size) of `s` at addr (NULL = anywhere). offset/addr/size: granularity aligned.
 * If addr lies inside a HOST_RESERVE_PLACEHOLDER reservation, the view replaces that part of it.
 * Returns the view address or NULL. */
void *host_section_map(host_section *s, uint64_t offset, size_t size, void *addr, int prot);
/* Unmap a view. If it was mapped into a placeholder reservation the range returns to being reserved
 * (keep_reserved != 0) so it can be mapped again; otherwise the address range is freed. */
int host_section_unmap(void *view, size_t size, int keep_reserved);

#ifdef __cplusplus
}
#endif
#endif
