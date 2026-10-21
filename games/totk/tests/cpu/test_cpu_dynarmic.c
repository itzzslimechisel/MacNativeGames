/* CPU backend test: hand-assembled AArch64 run through cpu_backend.h (dynarmic on x86-64).
 * Covers: arithmetic loop, loads/stores on identity-mapped memory, svc (+ halt request), host traps with int and
 * float marshalling, re-entrant host->guest calls (recursion through the host), LDXR/STXR / CLREX and a
 * 4-thread atomic counter, TPIDR/TPIDRRO/CNTPCT, cache invalidation after code patching. */
#include "cpu_backend.h"
#include "host_mem.h"
#include "host_thread.h"
#include "test_common.h"
#include <stdint.h>
#include <string.h>

/* ---- tiny A64 assembler ---- */
static uint32_t *pc_;
static uint64_t here(void) { return (uint64_t)(uintptr_t)pc_; }
static void emit(uint32_t i) { *pc_++ = i; }
#define MOVZ(d, imm, hw) emit(0xD2800000u | ((hw) << 21) | (((imm)&0xFFFF) << 5) | (d))
#define MOVK(d, imm, hw) emit(0xF2800000u | ((hw) << 21) | (((imm)&0xFFFF) << 5) | (d))
#define ADDI(d, n, imm) emit(0x91000000u | ((imm) << 10) | ((n) << 5) | (d))
#define SUBI(d, n, imm) emit(0xD1000000u | ((imm) << 10) | ((n) << 5) | (d))
#define ADDW_I(d, n, imm) emit(0x11000000u | ((imm) << 10) | ((n) << 5) | (d))
#define ADD(d, n, m) emit(0x8B000000u | ((m) << 16) | ((n) << 5) | (d))
#define MUL(d, n, m) emit(0x9B007C00u | ((m) << 16) | ((n) << 5) | (d))
#define MOV(d, m) emit(0xAA0003E0u | ((m) << 16) | (d))
#define LDR(t, n, off) emit(0xF9400000u | (((off) / 8) << 10) | ((n) << 5) | (t))
#define STR(t, n, off) emit(0xF9000000u | (((off) / 8) << 10) | ((n) << 5) | (t))
#define LDRW(t, n, off) emit(0xB9400000u | (((off) / 4) << 10) | ((n) << 5) | (t))
#define STRW(t, n, off) emit(0xB9000000u | (((off) / 4) << 10) | ((n) << 5) | (t))
#define SVC(imm) emit(0xD4000001u | ((imm) << 5))
#define RET() emit(0xD65F03C0u)
#define BLR(n) emit(0xD63F0000u | ((n) << 5))
#define PUSH_FP_LR() emit(0xA9BF7BFDu) /* stp x29, x30, [sp, #-16]! */
#define POP_FP_LR() emit(0xA8C17BFDu)  /* ldp x29, x30, [sp], #16 */
#define STP_X19_X20() emit(0xA9BF53F3u) /* stp x19, x20, [sp, #-16]! */
#define LDP_X19_X20() emit(0xA8C153F3u) /* ldp x19, x20, [sp], #16 */
#define MRS_TPIDR(t) emit(0xD53BD040u | (t))
#define MRS_TPIDRRO(t) emit(0xD53BD060u | (t))
#define MSR_TPIDR(t) emit(0xD51BD040u | (t))
#define MRS_CNTPCT(t) emit(0xD53BE020u | (t))
#define LDAXR(t, n) emit(0xC85FFC00u | ((n) << 5) | (t))
#define STLXR(s, t, n) emit(0xC800FC00u | ((s) << 16) | ((n) << 5) | (t))
#define LDXR(t, n) emit(0xC85F7C00u | ((n) << 5) | (t))
#define STXR(s, t, n) emit(0xC8007C00u | ((s) << 16) | ((n) << 5) | (t))
#define CLREX() emit(0xD5033F5Fu)
#define FMOV_D_X(d, n) emit(0x9E670000u | ((n) << 5) | (d))
#define FMOV_X_D(d, n) emit(0x9E660000u | ((n) << 5) | (d))
static void cbnz_to(int t, uint64_t target, int is64) {
    int64_t off = ((int64_t)target - (int64_t)here()) / 4;
    emit((is64 ? 0xB5000000u : 0x35000000u) | (((uint32_t)off & 0x7FFFF) << 5) | t);
}
static void cbz_fwd(uint32_t *at, uint64_t target, int t) { /* patch a placeholder */
    int64_t off = ((int64_t)target - (int64_t)(uintptr_t)at) / 4;
    *at = 0xB4000000u | (((uint32_t)off & 0x7FFFF) << 5) | t;
}
static void mov64(int d, uint64_t v) {
    MOVZ(d, v & 0xFFFF, 0);
    for (int hw = 1; hw < 4; hw++)
        if ((v >> (16 * hw)) & 0xFFFF) MOVK(d, (v >> (16 * hw)) & 0xFFFF, hw);
}

/* ---- callbacks ---- */
static uint32_t g_last_svc;
static int g_svc_count;
static void on_svc(cpu_context *ctx, uint32_t imm, void *user) {
    g_last_svc = imm;
    g_svc_count++;
    if (imm == 0x21) cpu_set_reg(ctx, 0, cpu_get_reg(ctx, 0) * 2);
    if (imm == 0x22) cpu_request_halt(ctx);
}
static uint64_t on_cntpct(void *user) { return 0x123456789ull; }
static uint64_t g_abort_pc, g_abort_addr;
static int g_abort_write;
static int on_abort(cpu_context *ctx, uint64_t pc, uint64_t addr, int is_write, void *user) {
    g_abort_pc = pc; g_abort_addr = addr; g_abort_write = is_write;
    cpu_set_reg(ctx, 0, 0x77);              /* recovery: skip the faulting load with a value of our own */
    cpu_set_reg(ctx, CPU_REG_PC, pc + 4);
    return 1;
}

static uint64_t host_madd(uint64_t a, uint64_t b, uint64_t c) { return a + b * c; }
static uint64_t host_sum8(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g,
                          uint64_t h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
static double host_scale(uint64_t n, double d) { return (double)n * d; }
static float host_mixf(float a, uint64_t n, float b) { return a * (float)n + b; }

static uint64_t g_fact_fn;
static uint64_t host_fact(uint64_t n) { /* guest fact(n) -> host_fact(n-1) -> guest fact(n-1) ... */
    uint64_t a = n;
    int ok = 0;
    uint64_t r = cpu_call_guest(cpu_current(), g_fact_fn, &a, 1, &ok);
    return ok ? r : 0xDEAD;
}

/* ---- atomics threads ---- */
static uint64_t g_atomic_fn, g_counter_addr, g_stack_base;
enum { NTHREADS = 4, ITERS = 100000 };
static void *atomic_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    cpu_context *c = cpu_context_create((uint32_t)id + 1, NULL);
    if (!c) return (void *)1;
    cpu_set_reg(c, CPU_REG_SP, g_stack_base + (uint64_t)(id + 1) * 0x10000);
    uint64_t args[2] = {g_counter_addr, ITERS};
    int ok = 0;
    cpu_call_guest(c, g_atomic_fn, args, 2, &ok);
    cpu_context_destroy(c);
    return ok ? NULL : (void *)1;
}

int main(void) {
    cpu_backend_config cfg = {0};
    cfg.svc = on_svc;
    cfg.max_contexts = 8;
    cfg.data_abort = on_abort;
    cfg.precise_aborts = 1;
    REQUIRE(cpu_backend_init(&cfg) == 0);
    printf("backend: %s\n", cpu_backend_name());

    const size_t SZ = 4u << 20;
    uint8_t *mem = host_mem_reserve_in_range(1ull << 33, 1ull << 39, SZ, 0);
    REQUIRE(mem && host_mem_commit(mem, SZ, HOST_PROT_RW) == 0);
    uint8_t *data = mem + (1u << 20);
    g_stack_base = (uint64_t)(uintptr_t)(mem + (2u << 20));
    pc_ = (uint32_t *)mem;

    cpu_context *ctx = cpu_context_create(0, NULL);
    REQUIRE(ctx);
    const uint64_t stack_top = g_stack_base + 0x10000;
    cpu_set_reg(ctx, CPU_REG_SP, stack_top);

    /* 1. arithmetic loop: sum(n) */
    uint64_t f_sum = here();
    MOVZ(1, 0, 0);
    uint64_t loop = here();
    ADD(1, 1, 0);
    SUBI(0, 0, 1);
    cbnz_to(0, loop, 1);
    MOV(0, 1);
    RET();
    uint64_t a1 = 1000;
    int ok = 0;
    CHECK(cpu_call_guest(ctx, f_sum, &a1, 1, &ok) == 500500 && ok);
    /* same through cpu_run with LR = return sentinel */
    cpu_set_reg(ctx, 0, 10);
    cpu_set_reg(ctx, CPU_REG_LR, cpu_return_address());
    cpu_set_reg(ctx, CPU_REG_PC, f_sum);
    CHECK(cpu_run(ctx) == CPU_HALT_RETURNED);
    CHECK(cpu_get_reg(ctx, 0) == 55);
    CHECK(cpu_get_reg(ctx, CPU_REG_SP) == stack_top);

    /* 2. memory: p[2] = p[0] + p[1]; ((u32*)p)[7] = ((u32*)p)[6] + 5 */
    uint64_t f_mem = here();
    LDR(2, 0, 0);
    LDR(3, 0, 8);
    ADD(2, 2, 3);
    STR(2, 0, 16);
    LDRW(4, 0, 24);
    ADDW_I(4, 4, 5);
    STRW(4, 0, 28);
    MOV(0, 2);
    RET();
    uint64_t *p = (uint64_t *)data;
    p[0] = 0x1111111122222222ull;
    p[1] = 0x0000000100000001ull;
    ((uint32_t *)p)[6] = 0xFFFFFFFE;
    uint64_t a2 = (uint64_t)(uintptr_t)p;
    CHECK(cpu_call_guest(ctx, f_mem, &a2, 1, &ok) == 0x1111111222222223ull && ok);
    CHECK(p[2] == 0x1111111222222223ull);
    CHECK(((uint32_t *)p)[7] == 3); /* 32-bit wrap */

    /* 3. svc: handler doubles x0 */
    uint64_t f_svc = here();
    MOVZ(0, 41, 0);
    SVC(0x21);
    ADDI(0, 0, 1);
    RET();
    CHECK(cpu_call_guest(ctx, f_svc, NULL, 0, &ok) == 83 && ok);
    CHECK(g_last_svc == 0x21);
    /* svc that requests a halt: cpu_run stops after it and resumes at the next instruction */
    uint64_t f_svc_halt = here();
    MOVZ(0, 7, 0);
    SVC(0x22);
    ADDI(0, 0, 3);
    RET();
    cpu_set_reg(ctx, CPU_REG_LR, cpu_return_address());
    cpu_set_reg(ctx, CPU_REG_PC, f_svc_halt);
    CHECK(cpu_run(ctx) == CPU_HALT_REQUESTED);
    CHECK(cpu_get_reg(ctx, 0) == 7 && cpu_get_reg(ctx, CPU_REG_PC) == f_svc_halt + 8);
    CHECK(cpu_run(ctx) == CPU_HALT_RETURNED);
    CHECK(cpu_get_reg(ctx, 0) == 10);

    /* 4. host traps */
    uint64_t t_madd = cpu_alloc_host_trap((void *)host_madd, "i:iii");
    uint64_t t_sum8 = cpu_alloc_host_trap((void *)host_sum8, "i:iiiiiiii");
    uint64_t t_scale = cpu_alloc_host_trap((void *)host_scale, "d:id");
    uint64_t t_mixf = cpu_alloc_host_trap((void *)host_mixf, "f:fif");
    REQUIRE(t_madd && t_sum8 && t_scale && t_mixf);
    CHECK(cpu_alloc_host_trap((void *)host_madd, "i:iiiiiiiii") == 0); /* 9 args rejected */
    /* guest: r = madd(x0, x1, x2) + 1 via blr x3 */
    uint64_t f_trap = here();
    PUSH_FP_LR();
    BLR(3);
    ADDI(0, 0, 1);
    POP_FP_LR();
    RET();
    uint64_t a4[4] = {5, 6, 7, t_madd};
    CHECK(cpu_call_guest(ctx, f_trap, a4, 4, &ok) == 48 && ok);
    /* 8 int args: load x0..x7 with 1..8 and call */
    uint64_t f_trap8 = here();
    PUSH_FP_LR();
    mov64(9, t_sum8);
    for (int i = 0; i < 8; i++) MOVZ(i, i + 1, 0);
    BLR(9);
    POP_FP_LR();
    RET();
    CHECK(cpu_call_guest(ctx, f_trap8, NULL, 0, &ok) == 204 && ok); /* sum i*i, i=1..8 */
    /* trap at a caller-chosen address (e.g. an import slot target) */
    uint64_t fixed_trap = (uint64_t)(uintptr_t)(mem + 0xF0000);
    CHECK(cpu_register_host_trap(fixed_trap, (void *)host_madd, "i:iii") == 0);
    uint64_t a4b[4] = {1, 2, 3, fixed_trap};
    CHECK(cpu_call_guest(ctx, f_trap, a4b, 4, &ok) == 8 && ok);
    /* float/double marshalling: x0 = n, x1 = double bits -> d0, call, return d0 bits in x0 */
    uint64_t f_trapd = here();
    PUSH_FP_LR();
    FMOV_D_X(0, 1);
    BLR(2);
    FMOV_X_D(0, 0);
    POP_FP_LR();
    RET();
    double two5 = 2.5, r_d;
    uint64_t bits;
    memcpy(&bits, &two5, 8);
    uint64_t a5[3] = {3, bits, t_scale};
    uint64_t rb = cpu_call_guest(ctx, f_trapd, a5, 3, &ok);
    memcpy(&r_d, &rb, 8);
    CHECK(ok && r_d == 7.5);
    /* float args in s0, s1 with an int in between: mixf(1.5f, 4, 0.25f) = 6.25f */
    uint64_t f_trapf = here();
    PUSH_FP_LR();
    FMOV_D_X(0, 0); /* d0 = x0 (float bits in low half) */
    FMOV_D_X(1, 1);
    MOV(0, 2);
    BLR(3);
    FMOV_X_D(0, 0);
    POP_FP_LR();
    RET();
    float f15 = 1.5f, f025 = 0.25f, r_f;
    uint32_t b15, b025;
    memcpy(&b15, &f15, 4);
    memcpy(&b025, &f025, 4);
    uint64_t a6[4] = {b15, b025, 4, t_mixf};
    uint32_t rf = (uint32_t)cpu_call_guest(ctx, f_trapf, a6, 4, &ok);
    memcpy(&r_f, &rf, 4);
    CHECK(ok && r_f == 6.25f);

    /* 5. re-entrant host->guest: fact(n) = n == 0 ? 1 : n * host_fact(n - 1), host_fact calls guest fact */
    uint64_t t_fact = cpu_alloc_host_trap((void *)host_fact, "i:i");
    g_fact_fn = here();
    uint32_t *cbz_at = pc_;
    emit(0); /* cbz x0, base */
    PUSH_FP_LR();
    STP_X19_X20();
    MOV(19, 0);
    SUBI(0, 0, 1);
    mov64(9, t_fact);
    BLR(9);
    MUL(0, 0, 19);
    LDP_X19_X20();
    POP_FP_LR();
    RET();
    cbz_fwd(cbz_at, here(), 0);
    MOVZ(0, 1, 0);
    RET();
    uint64_t a7 = 6;
    CHECK(cpu_call_guest(ctx, g_fact_fn, &a7, 1, &ok) == 720 && ok);
    CHECK(cpu_get_reg(ctx, CPU_REG_SP) == stack_top); /* level-0 state restored */
    CHECK(cpu_current() == NULL);

    /* 6. exclusives */
    uint64_t f_excl = here(); /* x0 = ptr, x1 = 1 -> clrex between: returns stxr status */
    LDXR(2, 0);
    ADDI(2, 2, 1);
    uint32_t *cbz2 = pc_;
    emit(0); /* cbz x1, skip */
    CLREX();
    cbz_fwd(cbz2, here(), 1);
    STXR(3, 2, 0);
    MOV(0, 3);
    RET();
    uint64_t *cell = (uint64_t *)(data + 0x100);
    *cell = 41;
    uint64_t a8[2] = {(uint64_t)(uintptr_t)cell, 0};
    CHECK(cpu_call_guest(ctx, f_excl, a8, 2, &ok) == 0 && *cell == 42);
    a8[1] = 1;
    CHECK(cpu_call_guest(ctx, f_excl, a8, 2, &ok) == 1 && *cell == 42);
    /* 4 threads x 100000 LDAXR/STLXR increments, each on its own context */
    g_atomic_fn = here();
    uint64_t aloop = here();
    LDAXR(2, 0);
    ADDI(2, 2, 1);
    STLXR(3, 2, 0);
    cbnz_to(3, aloop, 0);
    SUBI(1, 1, 1);
    cbnz_to(1, aloop, 1);
    RET();
    g_counter_addr = (uint64_t)(uintptr_t)(data + 0x200);
    *(uint64_t *)(data + 0x200) = 0;
    host_thread *th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) REQUIRE(host_thread_create(&th[i], atomic_worker, (void *)(intptr_t)i, 0) == 0);
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = (void *)1;
        host_thread_join(th[i], &ret);
        CHECK(ret == NULL);
    }
    CHECK(*(uint64_t *)(data + 0x200) == (uint64_t)NTHREADS * ITERS);

    /* 7. system registers */
    uint64_t f_sys = here();
    MRS_TPIDRRO(0);
    MRS_TPIDR(1);
    ADD(0, 0, 1);
    MSR_TPIDR(0);
    MRS_CNTPCT(2);
    STR(2, 3, 0);
    RET();
    cpu_set_tpidrro(ctx, 0x1000);
    cpu_set_tpidr(ctx, 0x234);
    uint64_t a9[4] = {0, 0, 0, (uint64_t)(uintptr_t)(data + 0x300)};
    CHECK(cpu_call_guest(ctx, f_sys, a9, 4, &ok) == 0x1234 && ok);
    CHECK(cpu_get_tpidr(ctx) == 0x1234);
    CHECK(*(uint64_t *)(data + 0x300) != 0); /* default cntpct: host counter scaled */

    /* 8. code patch + invalidate */
    uint64_t f_patch = here();
    uint32_t *patch_at = pc_;
    MOVZ(0, 1, 0);
    RET();
    CHECK(cpu_call_guest(ctx, f_patch, NULL, 0, &ok) == 1);
    *patch_at = 0xD2800000u | (2u << 5); /* movz x0, #2 */
    cpu_invalidate(f_patch, 4);
    CHECK(cpu_call_guest(ctx, f_patch, NULL, 0, &ok) == 2);

    /* 9. data abort: a load from unmapped memory reaches cfg.data_abort with the exact pc; the handler resumes after it */
    uint64_t f_abort = here();
    mov64(1, 0x10);
    uint64_t abort_ldr = here();
    LDR(0, 1, 0);
    RET();
    CHECK(cpu_call_guest(ctx, f_abort, NULL, 0, &ok) == 0x77 && ok);
    CHECK(g_abort_pc == abort_ldr && g_abort_addr == 0x10 && !g_abort_write);
    uint64_t abort_addr = 0; int abort_write = 1;
    CHECK(cpu_last_abort(ctx, &abort_addr, &abort_write) && abort_addr == 0x10 && !abort_write);

    cpu_context_destroy(ctx);
    cpu_backend_shutdown();

    /* cntpct callback */
    cfg.get_cntpct = on_cntpct;
    REQUIRE(cpu_backend_init(&cfg) == 0);
    ctx = cpu_context_create(0, NULL);
    cpu_set_reg(ctx, CPU_REG_SP, stack_top);
    CHECK(cpu_call_guest(ctx, f_sys, a9, 4, &ok) == 0 && ok); /* fresh context: tpidr regs 0 */
    CHECK(*(uint64_t *)(data + 0x300) == 0x123456789ull);
    cpu_context_destroy(ctx);
    cpu_backend_shutdown();
    host_mem_release(mem, SZ);
    TEST_DONE("cpu_dynarmic");
}
