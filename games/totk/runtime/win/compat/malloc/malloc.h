/* Windows stand-in for malloc_size. */
#pragma once
#include <malloc.h>
#define malloc_size(p) _msize(p)
