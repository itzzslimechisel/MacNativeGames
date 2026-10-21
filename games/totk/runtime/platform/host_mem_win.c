/* host_mem_win.c - host_mem.h on Windows (x86-64 and ARM64).
 * Reservations: VirtualAlloc(MEM_RESERVE) or VirtualAlloc2 placeholders. Aliasing: pagefile-backed sections
 * mapped with MapViewOfFile3(MEM_REPLACE_PLACEHOLDER) into placeholder ranges, or MapViewOfFileEx elsewhere.
 * Placeholder ranges hold section views only (they cannot be committed with host_mem_commit). */
#include "host_mem.h"
#include "host_win_internal.h"
#include <stdlib.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

typedef PVOID(WINAPI *pfn_VirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *pfn_MapViewOfFile3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL(WINAPI *pfn_UnmapViewOfFile2)(HANDLE, PVOID, ULONG);

static pfn_VirtualAlloc2 p_VirtualAlloc2;
static pfn_MapViewOfFile3 p_MapViewOfFile3;
static pfn_UnmapViewOfFile2 p_UnmapViewOfFile2;
static SYSTEM_INFO g_si;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK mem_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    GetSystemInfo(&g_si);
    p_VirtualAlloc2 = (pfn_VirtualAlloc2)(void *)host_win_proc("VirtualAlloc2");
    p_MapViewOfFile3 = (pfn_MapViewOfFile3)(void *)host_win_proc("MapViewOfFile3");
    p_UnmapViewOfFile2 = (pfn_UnmapViewOfFile2)(void *)host_win_proc("UnmapViewOfFile2");
    return TRUE;
}
static void init(void) { InitOnceExecuteOnce(&g_once, mem_init, NULL, NULL); }

static DWORD win_prot(int prot) {
    switch (prot & HOST_PROT_RWX) {
    case HOST_PROT_NONE: return PAGE_NOACCESS;
    case HOST_PROT_R: return PAGE_READONLY;
    case HOST_PROT_W:
    case HOST_PROT_RW: return PAGE_READWRITE;
    case HOST_PROT_X: return PAGE_EXECUTE;
    case HOST_PROT_RX: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

size_t host_page_size(void) { init(); return g_si.dwPageSize; }
size_t host_alloc_granularity(void) { init(); return g_si.dwAllocationGranularity; }

void *host_mem_reserve(void *addr, size_t size, int flags) {
    init();
    void *r = NULL;
    if (flags & HOST_RESERVE_PLACEHOLDER) {
        if (!p_VirtualAlloc2) return NULL;
        r = p_VirtualAlloc2(GetCurrentProcess(), addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        if (!r && addr && !(flags & HOST_RESERVE_FIXED))
            r = p_VirtualAlloc2(GetCurrentProcess(), NULL, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    } else {
        r = VirtualAlloc(addr, size, MEM_RESERVE, PAGE_NOACCESS);
        if (!r && addr && !(flags & HOST_RESERVE_FIXED)) r = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (r && addr && (flags & HOST_RESERVE_FIXED) && r != addr) { VirtualFree(r, 0, MEM_RELEASE); return NULL; }
    return r;
}

void *host_mem_reserve_in_range(uint64_t lo, uint64_t hi, size_t size, int flags) {
    init();
    uint64_t g = g_si.dwAllocationGranularity;
    uint64_t a = (lo + g - 1) & ~(g - 1);
    if (a < (uint64_t)(uintptr_t)g_si.lpMinimumApplicationAddress) a = ((uint64_t)(uintptr_t)g_si.lpMinimumApplicationAddress + g - 1) & ~(g - 1);
    while (a + size <= hi && a + size > a) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) return NULL;
        uint64_t rb = (uint64_t)(uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
        if (mbi.State == MEM_FREE && re >= a + size) {
            void *r = host_mem_reserve((void *)(uintptr_t)a, size, flags | HOST_RESERVE_FIXED);
            if (r) return r;
        }
        a = (re + g - 1) & ~(g - 1);
    }
    return NULL;
}

int host_mem_release(void *addr, size_t size) {
    /* Walk the range allocation by allocation: placeholders may have been split and views mapped. */
    uintptr_t p = (uintptr_t)addr, end = p + size;
    int rc = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return -1;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State != MEM_FREE && (uintptr_t)mbi.AllocationBase == p) {
            if (mbi.Type == MEM_MAPPED) { if (!UnmapViewOfFile((void *)p)) rc = -1; }
            else if (!VirtualFree((void *)p, 0, MEM_RELEASE)) rc = -1;
            if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) return -1;
            next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
        p = next;
    }
    return rc;
}

int host_mem_commit(void *addr, size_t size, int prot) {
    return VirtualAlloc(addr, size, MEM_COMMIT, win_prot(prot)) ? 0 : -1;
}

int host_mem_decommit(void *addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.Type == MEM_MAPPED) {
        /* Section views cannot be decommitted: zero and make inaccessible (same observable result). */
        DWORD old;
        if (!VirtualProtect(addr, size, PAGE_READWRITE, &old)) return -1;
        memset(addr, 0, size);
        return VirtualProtect(addr, size, PAGE_NOACCESS, &old) ? 0 : -1;
    }
    return VirtualFree(addr, size, MEM_DECOMMIT) ? 0 : -1;
}

int host_mem_protect(void *addr, size_t size, int prot) {
    DWORD old;
    return VirtualProtect(addr, size, win_prot(prot), &old) ? 0 : -1;
}

void host_flush_icache(void *addr, size_t size) { FlushInstructionCache(GetCurrentProcess(), addr, size); }

struct host_section { HANDLE h; size_t size; };

host_section *host_section_create(size_t size) {
    init();
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                  (DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
    if (!h) return NULL;
    host_section *s = (host_section *)malloc(sizeof *s);
    if (!s) { CloseHandle(h); return NULL; }
    s->h = h; s->size = size;
    return s;
}

void host_section_destroy(host_section *s) {
    if (!s) return;
    CloseHandle(s->h);
    free(s);
}

static DWORD view_access(int prot) {
    DWORD a = 0;
    if (prot & HOST_PROT_W) a |= FILE_MAP_WRITE; else if (prot & HOST_PROT_R) a |= FILE_MAP_READ;
    if (prot & HOST_PROT_X) a |= FILE_MAP_EXECUTE;
    return a ? a : FILE_MAP_READ;
}

void *host_section_map(host_section *s, uint64_t offset, size_t size, void *addr, int prot) {
    init();
    if (!s || offset + size > s->size) return NULL;
    void *v = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    int in_placeholder = addr && p_MapViewOfFile3 && VirtualQuery(addr, &mbi, sizeof mbi) &&
                         mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE;
    if (in_placeholder) {
        /* Carve [addr, addr+size) out as its own placeholder unless it already is exactly one. */
        if (!((uintptr_t)mbi.AllocationBase == (uintptr_t)addr && mbi.RegionSize == size)) {
            if (!VirtualFree(addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return NULL;
        }
        v = p_MapViewOfFile3(s->h, GetCurrentProcess(), addr, offset, size, MEM_REPLACE_PLACEHOLDER,
                             win_prot(prot ? prot : HOST_PROT_R), NULL, 0);
        if (!v) return NULL;
        return v;
    }
    v = MapViewOfFileEx(s->h, view_access(prot), (DWORD)(offset >> 32), (DWORD)offset, size, addr);
    if (!v) return NULL;
    if (prot == HOST_PROT_NONE || (prot & HOST_PROT_RWX) == HOST_PROT_X) {
        DWORD old; VirtualProtect(v, size, win_prot(prot), &old);
    }
    return v;
}

int host_section_unmap(void *view, size_t size, int keep_reserved) {
    (void)size;
    init();
    if (keep_reserved && p_UnmapViewOfFile2 && p_UnmapViewOfFile2(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER))
        return 0;
    if (!UnmapViewOfFile(view)) return -1;
    if (keep_reserved && !VirtualAlloc(view, size, MEM_RESERVE, PAGE_NOACCESS)) return -1;
    return 0;
}
