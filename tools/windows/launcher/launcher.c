/* "Tears of the Kingdom.exe": GUI-subsystem launcher for a package folder (see build_windows.py).
   Does what "<name>.bat" does: reads settings.bat, sets the TOTK_* environment, starts bin\totk-win.exe with stderr in
   logs\totk.log, waits, and reports problems in a MessageBox. Checks first: game data, .NET 10 runtime, Vulkan 1.3. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#define TITLE L"Tears of the Kingdom"
#define MAXP 1024
static wchar_t here[MAXP];            /* package folder with trailing backslash */
static wchar_t data[MAXP], xlate[MAXP] = L"dotnet.exe";
static wchar_t dispver[64], vdir[64];

static void fail(const wchar_t *fmt, ...) {
    wchar_t buf[4096]; va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 4095, fmt, ap); va_end(ap); buf[4095] = 0;
    MessageBoxW(NULL, buf, TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    ExitProcess(1);
}
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static wchar_t *readfile(const wchar_t *path) {   /* UTF-8 / ASCII text -> wide, NUL-terminated; NULL if missing */
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0; char *b = HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!b || !ReadFile(h, b, n, &got, NULL)) { CloseHandle(h); return NULL; }
    CloseHandle(h);
    int off = (got >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB) ? 3 : 0;
    wchar_t *w = HeapAlloc(GetProcessHeap(), 0, (got + 1) * sizeof(wchar_t));
    int m = MultiByteToWideChar(CP_UTF8, 0, b + off, got - off, w, got);
    w[m] = 0; HeapFree(GetProcessHeap(), 0, b);
    return w;
}

/* expand %~dp0 in a settings value */
static void expand(const wchar_t *in, wchar_t *out) {
    out[0] = 0;
    for (const wchar_t *p = in; *p;) {
        if (!wcsncmp(p, L"%~dp0", 5)) { wcscat(out, here); p += 5; }
        else { size_t l = wcslen(out); out[l] = *p++; out[l + 1] = 0; }
    }
}

/* settings.bat: lines   set "NAME=VALUE"   (not rem) -> environment, like the .bat's `call settings.bat` */
static void settings(void) {
    wchar_t path[MAXP]; swprintf(path, MAXP, L"%lssettings.bat", here);
    wchar_t *t = readfile(path), *ctx = NULL;
    if (!t) fail(L"settings.bat was not found next to the launcher:\n%ls", path);
    for (wchar_t *line = wcstok(t, L"\n", &ctx); line; line = wcstok(NULL, L"\n", &ctx)) {
        while (*line == L' ' || *line == L'\t') line++;
        if (_wcsnicmp(line, L"set \"", 5)) continue;
        wchar_t *name = line + 5, *eq = wcschr(name, L'='), *end = wcsrchr(line, L'"');
        if (!eq || !end || end < eq) continue;
        *eq = 0; *end = 0;
        wchar_t val[MAXP * 2]; expand(eq + 1, val);
        SetEnvironmentVariableW(name, val);
        if (!_wcsicmp(name, L"TOTK_DATA")) wcsncpy(data, val, MAXP - 1);
        if (!_wcsicmp(name, L"TOTK_XLATE")) wcsncpy(xlate, val, MAXP - 1);
    }
}

static void check_dotnet(void) {
    wchar_t exe[MAXP], dir[MAXP], pat[MAXP];
    WIN32_FIND_DATAW fd; HANDLE h;
    if (_wcsicmp(xlate, L"dotnet.exe") && _wcsicmp(xlate, L"dotnet")) return;   /* custom translator command: not ours to check */
    if (!SearchPathW(NULL, L"dotnet.exe", NULL, MAXP, exe, NULL)) {
        wchar_t pf[MAXP]; if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAXP)) wcscpy(pf, L"C:\\Program Files");
        swprintf(exe, MAXP, L"%ls\\dotnet\\dotnet.exe", pf);
        if (!exists(exe)) goto missing;
    }
    wcscpy(dir, exe); *wcsrchr(dir, L'\\') = 0;
    swprintf(pat, MAXP, L"%ls\\shared\\Microsoft.NETCore.App\\10.*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return; }
missing:
    if (MessageBoxW(NULL, L"The .NET 10 runtime is not installed. The game needs it to translate shaders.\n\n"
        L"Install \".NET Runtime 10\" (x64) from https://dotnet.microsoft.com/download/dotnet/10.0 and start the game again.\n\n"
        L"Open the download page now?", TITLE, MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND) == IDYES)
        ShellExecuteW(NULL, L"open", L"https://dotnet.microsoft.com/download/dotnet/10.0", NULL, NULL, SW_SHOWNORMAL);
    ExitProcess(1);
}

/* Vulkan 1.3 device check with hand-declared minimal types (no SDK needed) */
typedef int (__stdcall *PFN_ci)(const void *, const void *, void **);
typedef void (__stdcall *PFN_di)(void *, const void *);
typedef int (__stdcall *PFN_ep)(void *, unsigned *, void **);
typedef void (__stdcall *PFN_gp)(void *, void *);
typedef void *(__stdcall *PFN_gipa)(void *, const char *);
static void check_vulkan(void) {
    const unsigned v13 = (1u << 22) | (3u << 12);
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) fail(L"No Vulkan driver was found (vulkan-1.dll is missing).\n\nInstall the latest graphics driver from NVIDIA, AMD or Intel; the game needs Vulkan 1.3.");
    PFN_gipa gipa = (PFN_gipa)GetProcAddress(vk, "vkGetInstanceProcAddr");
    PFN_ci ci = gipa ? (PFN_ci)gipa(NULL, "vkCreateInstance") : NULL;
    if (!ci) fail(L"The Vulkan loader is broken (vkCreateInstance not found). Reinstall your graphics driver.");
    struct { int sType; const void *pNext; const char *an; unsigned av; const char *en; unsigned ev; unsigned api; } app =
        { 0 /*APPLICATION_INFO*/, NULL, "totk-launcher", 1, NULL, 0, v13 };
    struct { int sType; const void *pNext; unsigned flags; const void *app; unsigned nl; const char *const *l; unsigned ne; const char *const *e; } info =
        { 1 /*INSTANCE_CREATE_INFO*/, NULL, 0, &app, 0, NULL, 0, NULL };
    void *inst = NULL; int r = ci(&info, NULL, &inst);
    if (r != 0 || !inst) fail(L"Vulkan 1.3 could not be initialised (error %d).\n\nUpdate your graphics driver; the game needs Vulkan 1.3.", r);
    PFN_ep ep = (PFN_ep)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_gp gp = (PFN_gp)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_di di = (PFN_di)gipa(inst, "vkDestroyInstance");
    void *devs[16]; unsigned n = 16; unsigned best = 0;
    if (ep && gp && ep(inst, &n, devs) >= 0)
        for (unsigned i = 0; i < n; i++) { static char props[2048]; gp(devs[i], props); unsigned v = *(unsigned *)props; if (v > best) best = v; }
    else n = 0;
    if (di) di(inst, NULL);
    if (best < v13)
        fail(L"No graphics card with Vulkan 1.3 support was found%ls.\n\nUpdate your graphics driver (the highest version found is %u.%u).",
             n ? L"" : L" (no Vulkan device at all)", best >> 22, (best >> 12) & 0x3ff);
}

static void setv(const wchar_t *k, const wchar_t *fmt, ...) {
    wchar_t b[MAXP * 2]; va_list ap; va_start(ap, fmt); _vsnwprintf(b, MAXP * 2 - 1, fmt, ap); va_end(ap); b[MAXP * 2 - 1] = 0;
    SetEnvironmentVariableW(k, b);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    wchar_t me[MAXP]; GetModuleFileNameW(NULL, me, MAXP);
    wcscpy(here, me); *(wcsrchr(here, L'\\') + 1) = 0;

    wchar_t gv[MAXP], *t; swprintf(gv, MAXP, L"%lsbin\\game-version.txt", here);
    if (!(t = readfile(gv)) || swscanf(t, L"%63ls %63ls", dispver, vdir) != 2)
        fail(L"bin\\game-version.txt is missing or damaged in\n%ls\n\nThe package looks incomplete; rebuild it with build_windows.py.", here);

    settings();
    wchar_t exe[MAXP]; swprintf(exe, MAXP, L"%lsbin\\totk-win.exe", here);
    if (!exists(exe)) fail(L"bin\\totk-win.exe is missing from\n%ls", here);

    wchar_t romfs[MAXP], exefs[MAXP], probe[MAXP];
    swprintf(romfs, MAXP, L"%ls\\extracted\\%ls\\romfs", data, vdir);
    swprintf(exefs, MAXP, L"%ls\\extracted\\%ls\\exefs", data, vdir);
    swprintf(probe, MAXP, L"%ls\\romfs.bin", romfs);
    if (!exists(probe))
        fail(L"The game data for Tears of the Kingdom %ls was not found:\n%ls\n\nEdit TOTK_DATA in settings.bat (in %ls) so it points at the folder that contains \"extracted\\%ls\".",
             dispver, probe, here, vdir);

    check_dotnet();
    check_vulkan();

    wchar_t logs[MAXP]; swprintf(logs, MAXP, L"%lslogs", here); CreateDirectoryW(logs, NULL);
    setv(L"TOTK_ROMFS", L"%ls", romfs);
    setv(L"TOTK_ROMFS_INDEX", L"%lsbin\\runtime-index.bin", here);
    setv(L"TOTK_PROFILE_DIR", L"%lsuser-data", here);
    setv(L"TOTK_SHADER_CACHE", L"%lsshader-cache", here);
    setv(L"TOTK_XLATE_CMD", L"\"%ls\" \"%lsbin\\translator\\totk_xlate.dll\"", xlate, here);
    wchar_t au[8]; if (GetEnvironmentVariableW(L"TOTK_AUDIO_OFF", au, 8) && !wcscmp(au, L"0")) SetEnvironmentVariableW(L"TOTK_AUDIO_OFF", NULL);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    wchar_t logp[MAXP]; swprintf(logp, MAXP, L"%ls\\totk.log", logs);
    HANDLE log = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = nul; si.hStdError = log;
    wchar_t line[MAXP * 3]; swprintf(line, MAXP * 3, L"\"%ls\" \"%ls\" boot", exe, exefs);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, here, &si, &pi))
        fail(L"Could not start bin\\totk-win.exe (error %lu).", GetLastError());
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    if (rc != 0) fail(L"The game exited with code %lu.\n\nSee the log:\n%ls", rc, logp);
    return 0;
}
