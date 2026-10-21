/* Windows stand-in for the CoreFoundation locale calls of the shared runtime (UI language, the game's language):
   the user's default locale name ("pt-BR") split into language and country. */
#pragma once
#include <stdint.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct { char language[16], country[16]; } __CFLocaleWin;
typedef __CFLocaleWin *CFLocaleRef;
typedef const char *CFStringRef;
typedef int CFStringEncoding;
#define kCFStringEncodingUTF8 0x08000100
#define kCFLocaleLanguageCode ((CFStringRef)"language")
#define kCFLocaleCountryCode ((CFStringRef)"country")
static inline CFLocaleRef CFLocaleCopyCurrent(void) {
    static __CFLocaleWin l;
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {0};
    char utf8[LOCALE_NAME_MAX_LENGTH * 3] = {0};
    if (!l.language[0] && GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 &&
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL) > 0) {
        size_t n = strcspn(utf8, "-_");
        if (n < sizeof l.language) memcpy(l.language, utf8, n);
        const char *last = strrchr(utf8, '-');
        if (last && strlen(last + 1) == 2) memcpy(l.country, last + 1, 2);
    }
    return &l;
}
static inline CFStringRef CFLocaleGetValue(CFLocaleRef l, CFStringRef key) {
    return !strcmp(key, "language") ? l->language : l->country;
}
static inline int CFStringGetCString(CFStringRef s, char *out, long cap, CFStringEncoding e) {
    (void)e; if (cap <= 0) return 0;
    strncpy(out, s, (size_t)cap - 1); out[cap - 1] = 0; return 1;
}
static inline void CFRelease(const void *p) { (void)p; }
