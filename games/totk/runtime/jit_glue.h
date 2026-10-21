/* jit_glue.h - the runtime on the dynarmic CPU backend (x86-64 hosts, TOTK_CPU_JIT): runtime/jit_glue.c.
 * docs/windows-port-plan.md C17-C19. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cpu_backend.h"

void jit_init(void);
/* Guest-visible memory owned by the host (thread environments, the main thread's stack, host threads' guest stacks):
   below 2^39 so the guest can hand it to IPC like any other address. 4 KiB aligned, zeroed. */
void *jit_guest_alloc(size_t size);
void jit_guest_free(void *p, size_t size);
/* The calling host thread's CPU context (created on first use); tls/tlr become its TPIDR_EL0 / TPIDRRO_EL0. */
cpu_context *jit_thread_context(void);
void jit_thread_set_env(uint64_t tls, uint64_t tlr);
void jit_thread_release(void);                 /* the calling thread's context and stack, at thread exit */
void jit_request_exit(void);                   /* svc ExitThread: leave the guest at the end of this svc */
/* Guest address of a host trap calling fn (deduplicated per fn); name selects the marshalling (hle_sig). */
uint64_t jit_host_trap(void *fn, const char *name);
const char *jit_host_sig(const char *name);
/* Print a guest register dump and call chain for ctx (why: one line) to stderr. */
void jit_report(cpu_context *ctx, const char *why);
