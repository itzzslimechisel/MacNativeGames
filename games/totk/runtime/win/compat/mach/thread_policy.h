/* Windows stand-in for Mach thread policies: real-time policies are not available (calls fail). */
#pragma once
#include "../mach_vm_compat.h"
typedef int boolean_t;
typedef int *thread_policy_t;
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 4
static inline kern_return_t thread_policy_set(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)flavor; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(mach_port_t t, int flavor, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)flavor; (void)p; (void)n; if (def) *def = 1; return KERN_FAILURE; }
