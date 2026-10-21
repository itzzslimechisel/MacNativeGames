/* host_fault_mac.c - host_fault.h on macOS/iOS: sigaction(SIGSEGV/SIGBUS/SIGILL/SIGTRAP) + ucontext, and
 * guarded reads through mach_vm_read_overwrite (what the runtime's safe reads use today).
 *
 * NOTE: runtime/loader.c installs its own handlers for the same signals. This file is not linked into the
 * macOS build yet; when the runtime adopts host_fault.h, loader.c's crash/CTR_EL0/fault-guard logic must
 * move into host_fault handlers instead of calling sigaction itself. */
#include "host_fault.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define MAX_HANDLERS 16
static host_fault_handler volatile g_handlers[MAX_HANDLERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static const int g_sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP};
static struct sigaction g_old[4];

static int sig_index(int sig) {
    for (int i = 0; i < 4; i++) if (g_sigs[i] == sig) return i;
    return -1;
}

static void on_signal(int sig, siginfo_t *si, void *uctx) {
    ucontext_t *uc = (ucontext_t *)uctx;
    host_cpu_context hc;
    host_fault_info info;
    memset(&hc, 0, sizeof hc);
    memset(&info, 0, sizeof info);
    info.kind = (sig == SIGSEGV || sig == SIGBUS) ? HOST_FAULT_ACCESS : sig == SIGILL ? HOST_FAULT_ILLEGAL : HOST_FAULT_BREAKPOINT;
    info.addr = (uint64_t)(uintptr_t)si->si_addr;
    info.access = HOST_ACCESS_UNKNOWN;
#if defined(__arm64__) || defined(__aarch64__)
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; i++) hc.gpr[i] = ss->__x[i];
    hc.gpr[29] = (uint64_t)arm_thread_state64_get_fp(*ss);
    hc.gpr[30] = (uint64_t)arm_thread_state64_get_lr(*ss);
    hc.sp = (uint64_t)arm_thread_state64_get_sp(*ss);
    hc.pc = (uint64_t)arm_thread_state64_get_pc(*ss);
    hc.flags = ss->__cpsr;
    hc.ngpr = 31;
    if (info.kind == HOST_FAULT_ACCESS) {
        uint32_t esr = uc->uc_mcontext->__es.__esr, ec = esr >> 26;
        if (ec == 0x24 || ec == 0x25) info.access = (esr & (1u << 6)) ? HOST_ACCESS_WRITE : HOST_ACCESS_READ;
        else if (ec == 0x20 || ec == 0x21) info.access = HOST_ACCESS_EXEC;
    }
#elif defined(__x86_64__)
    hc.pc = uc->uc_mcontext->__ss.__rip;
    hc.sp = uc->uc_mcontext->__ss.__rsp;
#endif
    info.ctx = &hc;
    info.native = uc;
    for (int i = 0; i < MAX_HANDLERS; i++) {
        host_fault_handler h = g_handlers[i];
        if (h && h(&info) == HOST_FAULT_HANDLED) {
#if defined(__arm64__) || defined(__aarch64__)
            for (int r = 0; r < 29; r++) ss->__x[r] = hc.gpr[r];
            arm_thread_state64_set_fp(*ss, hc.gpr[29]);
            arm_thread_state64_set_lr_fptr(*ss, (void *)(uintptr_t)hc.gpr[30]);
            arm_thread_state64_set_sp(*ss, hc.sp);
            arm_thread_state64_set_pc_fptr(*ss, (void *)(uintptr_t)hc.pc);
            ss->__cpsr = (uint32_t)hc.flags;
#elif defined(__x86_64__)
            uc->uc_mcontext->__ss.__rip = hc.pc;
            uc->uc_mcontext->__ss.__rsp = hc.sp;
#endif
            return;
        }
    }
    /* Unhandled: chain to the previous disposition. */
    int k = sig_index(sig);
    if (k >= 0 && (g_old[k].sa_flags & SA_SIGINFO) && g_old[k].sa_sigaction) {
        g_old[k].sa_sigaction(sig, si, uctx);
        return;
    }
    if (k >= 0 && g_old[k].sa_handler != SIG_DFL && g_old[k].sa_handler != SIG_IGN && g_old[k].sa_handler) {
        g_old[k].sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL); /* returning re-executes the faulting instruction -> default action */
}

static void install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 4; i++) sigaction(g_sigs[i], &sa, &g_old[i]);
}

int host_fault_add_handler(host_fault_handler h) {
    if (!h) return -1;
    pthread_once(&g_once, install);
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (!g_handlers[i]) { g_handlers[i] = h; rc = 0; break; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int host_fault_remove_handler(host_fault_handler h) {
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (g_handlers[i] == h) { g_handlers[i] = NULL; rc = 0; break; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

size_t host_guarded_read(void *dst, const void *src, size_t n) {
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)src, n,
                               (mach_vm_address_t)(uintptr_t)dst, &got) == KERN_SUCCESS)
        return (size_t)got;
    /* Partial: copy page by page up to the first unreadable page. */
    size_t pg = (size_t)getpagesize(), done = 0;
    while (done < n) {
        uintptr_t s = (uintptr_t)src + done;
        size_t chunk = pg - (s & (pg - 1));
        if (chunk > n - done) chunk = n - done;
        got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), s, chunk, (mach_vm_address_t)(uintptr_t)dst + done, &got) != KERN_SUCCESS)
            break;
        done += (size_t)got;
    }
    return done;
}

size_t host_guarded_write(void *dst, const void *src, size_t n) {
    /* mach_vm_write fails as a whole on an unmapped or read-only destination: write page by page. */
    size_t pg = (size_t)getpagesize(), done = 0;
    while (done < n) {
        uintptr_t d = (uintptr_t)dst + done;
        size_t chunk = pg - (d & (pg - 1));
        if (chunk > n - done) chunk = n - done;
        if (mach_vm_write(mach_task_self(), (mach_vm_address_t)d, (vm_offset_t)((uintptr_t)src + done), (mach_msg_type_number_t)chunk) != KERN_SUCCESS) break;
        done += chunk;
    }
    return done;
}
