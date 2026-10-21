// NVN entry points under the dynarmic CPU backend (TOTK_CPU_JIT, x86-64): the counterpart of the generated
// nvn_thunks.S / nvn_entry.S. Each NVN name the guest asks for is a host trap reading nvn_slot[i].fn at every call (the
// thunks' slot table), with its arguments marshalled by the signature tools/windows/gen_nvn_traps.py derives from
// nvn_protos.h (nvn_jit_sigs.h, generated into the build folder).
#include "nvn_count.h"
#include "nvn_jit_sigs.h"
#include "../../host/dm.h"
#include "cpu_backend.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
extern nvn_slot_t nvn_slot[NVN_NFUNC];
const char *nvn_names[NVN_NFUNC] = {NVN_JIT_NAME_LIST};
void *nvn_thunk[NVN_NFUNC];   // unused under the JIT (nvn_jit_entry hands out the traps)

static uint64_t g_trap[NVN_NFUNC];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// Unimplemented NVN function called: the trap that led here names it.
uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2);
uint64_t nvn_unimpl_entry(uint64_t x0, uint64_t x1, uint64_t x2) {
    cpu_context *c = cpu_current();
    uint64_t at = c ? cpu_get_reg(c, CPU_REG_PC) - 4 : 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (g_trap[i] && g_trap[i] == at) return nvn_unimpl_c(&nvn_slot[i], x0, x1, x2);
    die("unimplemented NVN call (entry %#llx)", (unsigned long long)at);
}
void nvn_prof_entry(void) { die("DM_NVN_PROF is not available with the JIT backend"); }

void *nvn_jit_entry(unsigned i) {
    pthread_mutex_lock(&g_mu);
    if (!g_trap[i]) {
        const char *sig = nvn_jit_sigs[i] ? nvn_jit_sigs[i] : "i:iiiiiiii";   // no prototype: no floating-point arguments either
        g_trap[i] = cpu_alloc_host_trap_indirect((void *const *)&nvn_slot[i].fn, sig);
        if (!g_trap[i]) die("NVN host trap for %s", nvn_names[i]);
    }
    uint64_t a = g_trap[i];
    pthread_mutex_unlock(&g_mu);
    return (void *)(uintptr_t)a;
}
// NVN_EXTRA entries (not in the name table): integer arguments only (checked in totk_nvn_ext.c).
void *nvn_jit_extra(void *fn, const char *name) {
    uint64_t jit_host_trap(void *fn, const char *name);
    return (void *)(uintptr_t)jit_host_trap(fn, name);
}
