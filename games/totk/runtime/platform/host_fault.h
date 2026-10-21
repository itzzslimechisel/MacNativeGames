/* host_fault.h - synchronous CPU fault handling (macOS: sigaction + ucontext, Windows: vectored exception
 * handler + CONTEXT) and guarded reads of possibly-unmapped memory (replaces runtime_fault_guard /
 * mach_vm_read_overwrite safe reads).
 *
 * Handlers run on the faulting thread, inside the OS fault dispatch: no locks, no allocation. A handler may
 * edit info->ctx (pc, sp, gpr) and return HOST_FAULT_HANDLED to resume there; HOST_FAULT_CONTINUE passes the
 * fault to the next handler and finally to the OS / previous handler (crash). Never longjmp out of a handler. */
#ifndef HOST_FAULT_H
#define HOST_FAULT_H

#include <stddef.h>
#include <stdint.h>
#include "host_thread.h" /* host_cpu_context */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOST_FAULT_ACCESS = 1,  /* SIGSEGV/SIGBUS, EXCEPTION_ACCESS_VIOLATION / IN_PAGE_ERROR */
    HOST_FAULT_ILLEGAL,     /* SIGILL, EXCEPTION_ILLEGAL_INSTRUCTION / PRIV_INSTRUCTION */
    HOST_FAULT_BREAKPOINT,  /* SIGTRAP, EXCEPTION_BREAKPOINT */
    HOST_FAULT_OTHER,
} host_fault_kind;

typedef enum { HOST_ACCESS_READ = 0, HOST_ACCESS_WRITE = 1, HOST_ACCESS_EXEC = 2, HOST_ACCESS_UNKNOWN = 3 } host_access;

typedef struct host_fault_info {
    host_fault_kind kind;
    host_access access;     /* HOST_FAULT_ACCESS only */
    uint64_t addr;          /* faulting data address (ACCESS) or pc */
    host_cpu_context *ctx;  /* editable */
    void *native;           /* ucontext_t* or EXCEPTION_POINTERS* */
} host_fault_info;

enum { HOST_FAULT_CONTINUE = 0, HOST_FAULT_HANDLED = 1 };
typedef int (*host_fault_handler)(host_fault_info *info);

/* Handlers are called in registration order. Up to 16. Returns 0 or -1. */
int host_fault_add_handler(host_fault_handler h);
int host_fault_remove_handler(host_fault_handler h);

/* Copy n bytes from src to dst; unreadable source memory stops the copy instead of crashing.
 * Returns the number of bytes copied (n on full success). dst must be valid. */
size_t host_guarded_read(void *dst, const void *src, size_t n);
/* Copy n bytes to possibly unmapped / read-only dst; returns the number of bytes written (n on full success). */
size_t host_guarded_write(void *dst, const void *src, size_t n);

#ifdef __cplusplus
}
#endif
#endif
