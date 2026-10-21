/* Windows stand-in for the few Mach calls the shared runtime uses: guarded reads of possibly unmapped memory. */
#pragma once
#include "../mach_vm_compat.h"
