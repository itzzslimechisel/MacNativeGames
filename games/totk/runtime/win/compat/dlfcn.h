/* Windows stand-in for dlfcn.h: no symbol lookup of host code (diagnostics only). */
#pragma once
#include <stddef.h>
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
#define RTLD_DEFAULT ((void *)0)
static inline int dladdr(const void *addr, Dl_info *info) { (void)addr; (void)info; return 0; }
static inline void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return NULL; }
