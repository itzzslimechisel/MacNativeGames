/* host_fault_win.c - host_fault.h on Windows: one first-chance vectored exception handler.
 *
 * Guarded reads never longjmp (guest/JIT frames have no unwind data). The copy loop lives in a tiny asm routine
 * with a known faulting instruction; the VEH recognizes that pc and resumes at the routine's exit with the
 * remaining byte count still in a register:
 *   x86-64: `rep movsb` is restartable, rcx = bytes left at the fault.
 *   ARM64 : byte loop, x2 = bytes left (load is not post-indexed, so x1 is exact).
 * Requires a GNU-asm capable compiler (clang / clang-cl / gcc), not cl.exe. */
#include "host_fault.h"
#include "host_win_internal.h"

size_t host__guarded_copy(void *dst, const void *src, size_t n); /* returns bytes NOT copied */
extern char host__gc_fault_pc[], host__gc_resume_pc[];

#if defined(_M_X64) || defined(__x86_64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl host__guarded_copy\n"
    "host__guarded_copy:\n"      /* Win64: rcx = dst, rdx = src, r8 = n */
    "  pushq %rdi\n"
    "  pushq %rsi\n"
    "  movq %rcx, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  movq %r8, %rcx\n"
    ".globl host__gc_fault_pc\n"
    "host__gc_fault_pc:\n"
    "  rep movsb\n"
    ".globl host__gc_resume_pc\n"
    "host__gc_resume_pc:\n"
    "  movq %rcx, %rax\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  ret\n");
#define CTX_PC(c) ((c)->Rip)
#elif defined(_M_ARM64) || defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl host__guarded_copy\n"
    "host__guarded_copy:\n"      /* x0 = dst, x1 = src, x2 = n */
    "  cbz x2, host__gc_resume_pc\n"
    ".globl host__gc_fault_pc\n"
    "host__gc_fault_pc:\n"
    "  ldrb w3, [x1]\n"
    "  strb w3, [x0], #1\n"
    "  add x1, x1, #1\n"
    "  subs x2, x2, #1\n"
    "  b.ne host__gc_fault_pc\n"
    ".globl host__gc_resume_pc\n"
    "host__gc_resume_pc:\n"
    "  mov x0, x2\n"
    "  ret\n");
#define CTX_PC(c) ((c)->Pc)
#else
#error "unsupported Windows architecture"
#endif

#define MAX_HANDLERS 16
static host_fault_handler volatile g_handlers[MAX_HANDLERS];
static SRWLOCK g_lock = SRWLOCK_INIT;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD code = er->ExceptionCode;
    host_fault_info info;
    memset(&info, 0, sizeof info);
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        if (CTX_PC(c) == (DWORD64)(uintptr_t)host__gc_fault_pc) {
            CTX_PC(c) = (DWORD64)(uintptr_t)host__gc_resume_pc;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        info.kind = HOST_FAULT_ACCESS;
        info.access = HOST_ACCESS_UNKNOWN;
        if (er->NumberParameters >= 2) {
            ULONG_PTR a = er->ExceptionInformation[0];
            info.access = a == 0 ? HOST_ACCESS_READ : a == 1 ? HOST_ACCESS_WRITE : a == 8 ? HOST_ACCESS_EXEC : HOST_ACCESS_UNKNOWN;
            info.addr = er->ExceptionInformation[1];
        }
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: info.kind = HOST_FAULT_ILLEGAL; info.addr = CTX_PC(c); break;
    case EXCEPTION_BREAKPOINT: info.kind = HOST_FAULT_BREAKPOINT; info.addr = CTX_PC(c); break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: info.kind = HOST_FAULT_OTHER; info.addr = CTX_PC(c); break;
    default: return EXCEPTION_CONTINUE_SEARCH; /* C++ exceptions, debugger events, etc. */
    }
    host_cpu_context hc;
    host_ctx_from_native(c, &hc);
    info.ctx = &hc;
    info.native = ep;
    for (int i = 0; i < MAX_HANDLERS; i++) {
        host_fault_handler h = g_handlers[i];
        if (h && h(&info) == HOST_FAULT_HANDLED) {
            host_ctx_to_native(&hc, c);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK install(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    return AddVectoredExceptionHandler(1, veh) != NULL;
}
static int ensure_installed(void) { return InitOnceExecuteOnce(&g_once, install, NULL, NULL) ? 0 : -1; }

int host_fault_add_handler(host_fault_handler h) {
    if (!h || ensure_installed()) return -1;
    int rc = -1;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (!g_handlers[i]) { g_handlers[i] = h; rc = 0; break; }
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}

int host_fault_remove_handler(host_fault_handler h) {
    int rc = -1;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (g_handlers[i] == h) { g_handlers[i] = NULL; rc = 0; break; }
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}

size_t host_guarded_read(void *dst, const void *src, size_t n) {
    if (!n) return 0;
    if (ensure_installed()) return 0;
    return n - host__guarded_copy(dst, src, n);
}
size_t host_guarded_write(void *dst, const void *src, size_t n) {
    if (!n) return 0;
    if (ensure_installed()) return 0;
    return n - host__guarded_copy(dst, src, n);   /* a fault on either side stops the copy */
}
