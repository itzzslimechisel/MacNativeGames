/* Registration sections (NVN implementations, forced imports): Mach-O "__DATA,<name>" read back with getsectiondata;
   on Windows (COFF) "<name>$m", bracketed by "$a" / "$z" markers (runtime/win/win_sections.c). */
#pragma once
#ifdef _WIN32
#define DM_SECT(name) name "$m"
#else
#define DM_SECT(name) "__DATA," name
#endif
