/* host_win_internal.h - shared helpers for host_*_win.c (not part of the public interface). */
#ifndef HOST_WIN_INTERNAL_H
#define HOST_WIN_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include "host_thread.h"

/* Resolve an export from kernelbase.dll / kernel32.dll at runtime (APIs newer than the import libs). */
static inline FARPROC host_win_proc(const char *name) {
    HMODULE m = GetModuleHandleW(L"kernelbase.dll");
    FARPROC p = m ? GetProcAddress(m, name) : NULL;
    if (!p) { m = GetModuleHandleW(L"kernel32.dll"); p = m ? GetProcAddress(m, name) : NULL; }
    return p;
}

static inline void host_ctx_from_native(const CONTEXT *c, host_cpu_context *h) {
    memset(h, 0, sizeof *h);
#if defined(_M_X64) || defined(__x86_64__)
    h->pc = c->Rip; h->sp = c->Rsp; h->flags = c->EFlags; h->ngpr = 16;
    h->gpr[0] = c->Rax; h->gpr[1] = c->Rcx; h->gpr[2] = c->Rdx; h->gpr[3] = c->Rbx;
    h->gpr[4] = c->Rsp; h->gpr[5] = c->Rbp; h->gpr[6] = c->Rsi; h->gpr[7] = c->Rdi;
    h->gpr[8] = c->R8;  h->gpr[9] = c->R9;  h->gpr[10] = c->R10; h->gpr[11] = c->R11;
    h->gpr[12] = c->R12; h->gpr[13] = c->R13; h->gpr[14] = c->R14; h->gpr[15] = c->R15;
#elif defined(_M_ARM64) || defined(__aarch64__)
    h->pc = c->Pc; h->sp = c->Sp; h->flags = c->Cpsr; h->ngpr = 31;
    for (int i = 0; i < 31; i++) h->gpr[i] = c->X[i];
#else
#error "unsupported Windows architecture"
#endif
}

static inline void host_ctx_to_native(const host_cpu_context *h, CONTEXT *c) {
#if defined(_M_X64) || defined(__x86_64__)
    c->Rax = h->gpr[0]; c->Rcx = h->gpr[1]; c->Rdx = h->gpr[2]; c->Rbx = h->gpr[3];
    c->Rbp = h->gpr[5]; c->Rsi = h->gpr[6]; c->Rdi = h->gpr[7];
    c->R8 = h->gpr[8];  c->R9 = h->gpr[9];  c->R10 = h->gpr[10]; c->R11 = h->gpr[11];
    c->R12 = h->gpr[12]; c->R13 = h->gpr[13]; c->R14 = h->gpr[14]; c->R15 = h->gpr[15];
    c->Rip = h->pc; c->Rsp = h->sp; c->EFlags = (DWORD)h->flags; /* sp field wins over gpr[4] */
#elif defined(_M_ARM64) || defined(__aarch64__)
    for (int i = 0; i < 31; i++) c->X[i] = h->gpr[i];
    c->Pc = h->pc; c->Sp = h->sp; c->Cpsr = (DWORD)h->flags;
#endif
}

/* UTF-8 -> newly malloc'd UTF-16 (caller frees). long_path: prefix absolute drive paths with \\?\ and use '\'. */
wchar_t *host_win_widen(const char *utf8, int long_path);
/* UTF-16 -> UTF-8 into buf; returns length or -1. */
int host_win_narrow(const wchar_t *w, char *buf, size_t cap);

#endif
