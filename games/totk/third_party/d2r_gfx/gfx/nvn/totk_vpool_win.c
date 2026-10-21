// TOTK: NVN physical / virtual memory pools on Windows (totk_vpool.h; the macOS version is totk_vpool.c, Darwin VM
// remapping). docs/windows-port-plan.md C12.
// Aliasing needs section-backed memory: a physical pool the CPU cannot see (CPU_NO_ACCESS) gets a pagefile section as
// its storage, and a virtual pool is a placeholder reservation whose 64 KiB pages are views of physical sections (mapped)
// or of the virtual pool's own zero section (unmapped). A CPU-visible physical pool lives in game heap memory, which is
// not section-backed and cannot be aliased: mapping it into a virtual pool copies its current contents instead (logged
// once; later CPU writes through either view are not shared).
#include "totk_vpool.h"
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "../../host/hle.h"
#include "host_mem.h"
#include <pthread.h>

#define F_CPU_NO_ACCESS NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS
#define F_PHYSICAL NVN_MEMORY_POOL_FLAGS_PHYSICAL
#define F_VIRTUAL NVN_MEMORY_POOL_FLAGS_VIRTUAL
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')
#define PAGE TOTK_VPOOL_PAGE

static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint8_t *mem; uint64_t size; host_section *sec; uint8_t *mapped; } backing;   // mapped: per page, 1 = a view is there
static backing g_back[1024];
static int g_nback;

static backing *back_of(const uint8_t *mem) {
    for (int i = 0; i < g_nback; i++) if (g_back[i].mem == mem) return &g_back[i];
    return NULL;
}
static uint8_t *reserve_low(uint64_t size, int flags) {   // below 2^39 like all guest-visible memory
    return host_mem_reserve_in_range(1ull << 32, 1ull << 39, size, flags);
}

int totk_vpool_init(pool_t *o) {
    int phys = (o->flags & F_PHYSICAL) != 0, virt = (o->flags & F_VIRTUAL) != 0;
    o->game_mem = o->mem; o->host = NULL; o->span = 0;
    if (phys == virt) { LOG("【红】vpool: pool flags %#x must be exactly one of PHYSICAL/VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % PAGE) { LOG("【红】vpool: pool size %#llx not a multiple of 64 KiB", (unsigned long long)o->size); return 0; }
    if (virt && o->mem) { LOG("【红】vpool: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { LOG("【红】vpool: physical pool without storage"); return 0; }
    if (phys && !(o->flags & F_CPU_NO_ACCESS)) return 1;   // game storage; aliased by copy (see above)
    pthread_mutex_lock(&g_map_mu);
    if (g_nback == (int)(sizeof g_back / sizeof *g_back)) { pthread_mutex_unlock(&g_map_mu); LOG("【红】vpool: too many pools"); return 0; }
    backing *b = &g_back[g_nback];
    memset(b, 0, sizeof *b);
    b->size = o->size;
    b->sec = host_section_create(o->size);
    uint64_t pages = o->size / PAGE;
    if (b->sec && phys) b->mem = host_section_map(b->sec, 0, o->size, NULL, HOST_PROT_RW);
    if (b->sec && virt && (b->mem = reserve_low(o->size, HOST_RESERVE_PLACEHOLDER)) && (b->mapped = calloc(pages, 1))) {
        for (uint64_t p = 0; p < pages; p++)   // unmapped pages read as zero: the pool's own section
            if (!host_section_map(b->sec, p * PAGE, PAGE, b->mem + p * PAGE, HOST_PROT_RW)) { b->mem = NULL; break; }
            else b->mapped[p] = 1;
    }
    if (!b->mem) { pthread_mutex_unlock(&g_map_mu); LOG("【红】vpool: section storage for %#llx bytes failed", (unsigned long long)o->size); return 0; }
    g_nback++;
    pthread_mutex_unlock(&g_map_mu);
    o->span = o->size; o->host = b->mem; o->mem = b->mem;
    return 1;
}

int totk_vpool_fini(pool_t *o) {
    if (!o->host) return 0;
    pthread_mutex_lock(&g_map_mu);
    backing *b = back_of(o->host);
    if (b) {
        uint64_t pages = b->size / PAGE;
        if (b->mapped) {
            for (uint64_t p = 0; p < pages; p++) if (b->mapped[p]) host_section_unmap(b->mem + p * PAGE, PAGE, 1);
            host_mem_release(b->mem, b->size); free(b->mapped);
        } else host_section_unmap(b->mem, b->size, 0);
        host_section_destroy(b->sec);
        *b = g_back[--g_nback];
    }
    pthread_mutex_unlock(&g_map_mu);
    o->host = NULL; o->mem = o->game_mem; o->span = 0;
    return 1;
}

static int check_request(const pool_t *v, const totk_mapping_request *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) { LOG("【红】vpool: request %d negative/empty range", i); return 0; }
    if ((r->virtual_offset | r->physical_offset | r->size) % PAGE) { LOG("【红】vpool: request %d not 64 KiB aligned", i); return 0; }
    if (end < r->size || end > v->size) { LOG("【红】vpool: request %d exceeds virtual pool", i); return 0; }
    if (!r->physical_pool) return 1;
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != POOL_MAGIC || !(p->flags & F_PHYSICAL)) { LOG("【红】vpool: request %d source %p is not a physical pool", i, (const void *)p); return 0; }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) { LOG("【红】vpool: request %d exceeds physical pool", i); return 0; }
    return 1;
}

typedef struct { uintptr_t va, cpu; uint64_t size; } vmap_ent;
static vmap_ent g_vmap[512]; static int g_nvmap;
static void vmap_note(uintptr_t va, uint64_t size, uintptr_t cpu) {
    for (int i = 0; i < g_nvmap; i++) if (g_vmap[i].va == va) { g_vmap[i] = (vmap_ent){va, cpu, size}; return; }
    if (g_nvmap < 512) g_vmap[g_nvmap++] = (vmap_ent){va, cpu, size};
}
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) {
    pthread_mutex_lock(&g_map_mu); int r = 0;
    for (int i = 0; i < g_nvmap; i++) { const vmap_ent *e = &g_vmap[i]; if (e->cpu && va >= e->va && va + len <= e->va + e->size) { *cpu = e->cpu + (va - e->va); r = 1; break; } }
    pthread_mutex_unlock(&g_map_mu); return r;
}

static int map_page(backing *v, uint64_t p, host_section *sec, uint64_t off) {   // g_map_mu held
    uint8_t *at = v->mem + p * PAGE;
    if (v->mapped[p] && host_section_unmap(at, PAGE, 1)) return 0;
    v->mapped[p] = 0;
    if (!host_section_map(sec, off, PAGE, at, HOST_PROT_RW)) return 0;
    v->mapped[p] = 1;
    return 1;
}
static int apply_request(pool_t *v, backing *vb, const totk_mapping_request *r) {
    uint64_t first = (uint64_t)r->virtual_offset / PAGE, n = r->size / PAGE;
    uintptr_t va = (uintptr_t)(v->mem + r->virtual_offset);
    if (!r->physical_pool) {
        for (uint64_t k = 0; k < n; k++) if (!map_page(vb, first + k, vb->sec, (first + k) * PAGE)) return 0;
        memset(v->mem + r->virtual_offset, 0, r->size);
        vmap_note(va, r->size, 0);
        return 1;
    }
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    backing *pb = back_of(p->mem);
    if (pb && !pb->mapped) {   // section-backed physical pool: true aliasing
        for (uint64_t k = 0; k < n; k++) if (!map_page(vb, first + k, pb->sec, (uint64_t)r->physical_offset + k * PAGE)) return 0;
    } else {                   // CPU-visible physical pool in game memory: copy
        static int said;
        if (!said++) LOG("vpool: CPU-visible physical pool %p mapped into a virtual pool: copied, not aliased (Windows)", (void *)p);
        for (uint64_t k = 0; k < n; k++) if (!map_page(vb, first + k, vb->sec, (first + k) * PAGE)) return 0;
        memcpy(v->mem + r->virtual_offset, p->mem + r->physical_offset, r->size);
    }
    vmap_note(va, r->size, (uintptr_t)(p->mem + r->physical_offset));
    return 1;
}

int totk_vpool_map(pool_t *v, int count, const totk_mapping_request *r) {
    if (!v || v->magic != POOL_MAGIC || !(v->flags & F_VIRTUAL) || !v->mem) { LOG("【红】vpool: MapVirtual target %p is not a virtual pool", (void *)v); return 0; }
    if (count < 0 || (count && !r)) { LOG("【红】vpool: invalid request count %d", count); return 0; }
    for (int i = 0; i < count; i++) if (!check_request(v, &r[i], i)) return 0;
    pthread_mutex_lock(&g_map_mu);
    backing *vb = back_of(v->mem);
    if (!vb || !vb->mapped) { pthread_mutex_unlock(&g_map_mu); die("vpool: virtual pool %p has no placeholder backing", (void *)v); }
    nvn_dirty_remove_range(v->mem);
    mtl_pool_del(v->mem);
    int ok = 1;
    for (int i = 0; ok && i < count; i++) ok = apply_request(v, vb, &r[i]);
    mtl_pool_add(v->mem, v->size);
    nvn_dirty_add_range(v->mem, v->size);
    pthread_mutex_unlock(&g_map_mu);
    if (!ok) die("vpool: mapping failed after validation; virtual pool %p is inconsistent", (void *)v);
    return 1;
}
