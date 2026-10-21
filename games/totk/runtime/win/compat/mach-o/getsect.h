/* Windows stand-in for getsectiondata: the registration sections of dm_section.h (runtime/win/win_sections.c).
   Returns the entries without padding (a private copy), *size in bytes. */
#pragma once
const void *getsectiondata(const void *header, const char *segment, const char *section, unsigned long *size);
