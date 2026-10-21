/* Windows stand-in for mach_vm_read_overwrite (host_guarded_read) and the types around it. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "host_fault.h"
typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
static inline mach_port_t mach_task_self(void) { return 1; }
static inline kern_return_t mach_vm_read_overwrite(mach_port_t task, mach_vm_address_t src, mach_vm_size_t n,
                                                   mach_vm_address_t dst, mach_vm_size_t *got) {
    (void)task;
    size_t done = host_guarded_read((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
    if (got) *got = done;
    return done == n ? KERN_SUCCESS : KERN_FAILURE;
}
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }   /* no Mach ports: live thread dumps are skipped */
typedef uint64_t vm_address_t, vm_size_t;
static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t src, vm_size_t n, vm_address_t dst, vm_size_t *got) {
    return mach_vm_read_overwrite(task, src, n, dst, got);
}
/* Thread / task info queries (diagnostics): unavailable, the callers then skip their readings. */
typedef unsigned mach_msg_type_number_t;
typedef int *thread_info_t, *task_info_t;
typedef struct { struct { int seconds, microseconds; } user_time, system_time; } thread_basic_info_data_t;
typedef struct { int pth_curpri, pth_priority, pth_policy; } thread_extended_info_data_t;
typedef struct { uint64_t phys_footprint; } task_vm_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT 28
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 40
static inline mach_port_t mach_thread_self(void) { return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t p) { (void)task; (void)p; return KERN_SUCCESS; }
static inline kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n) { (void)t; (void)flavor; (void)out; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n) { (void)t; (void)flavor; (void)out; (void)n; return KERN_FAILURE; }
