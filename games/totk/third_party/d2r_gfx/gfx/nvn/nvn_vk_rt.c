// Vulkan NVN backend: runtime shader translation (the Windows counterpart of totk_shader_rt.m for the SPIR-V chain).
// A program stage becomes a variant "<shader>-<answers>": shader = hash of the control section (ctl_key_len bytes) and
// the code, answers = the bound-state facts the translator asks (texture slot types, image formats, vertex attribute and
// colour output classes). Variants are cached on disk as <cache>/<variant>_v.{spv,json}; a miss is translated by the
// service process totk_xlate --service --target spirv (TOTK_XLATE_CMD, e.g. "dotnet.exe" "totk_xlate.dll"), started once.
#include "nvn_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

int totk_depth_near_minus_w(void) __attribute__((weak));
static pthread_mutex_t g_rt_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[1024];
#ifdef _WIN32
static HANDLE g_in_w, g_out_r; static int g_svc = -1;
#endif
static uint64_t g_translated;

static const char *cache_dir(void) {
    if (g_dir[0]) return g_dir;
    const char *e = getenv("TOTK_SHADER_CACHE");
    if (e && *e) snprintf(g_dir, sizeof g_dir, "%s", e);
    else { const char *la = getenv("LOCALAPPDATA"); snprintf(g_dir, sizeof g_dir, "%s/MacNativeGames/TOTK/shader-cache-vk", la ? la : "."); }
#ifdef _WIN32
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", g_dir);
    for (char *p = tmp + 3; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = c; }
    CreateDirectoryA(tmp, NULL);
#endif
    return g_dir;
}
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static void hex(char *o, const uint8_t *b, size_t n) { static const char H[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { o[2 * i] = H[b[i] >> 4]; o[2 * i + 1] = H[b[i] & 15]; } o[2 * n] = 0; }

#ifdef _WIN32
static int svc_start(void) {
    if (g_svc >= 0) return g_svc;
    g_svc = 0;
    const char *cmd = getenv("TOTK_XLATE_CMD");
    if (!cmd || !*cmd) { VKLOG("TOTK_XLATE_CMD not set: shaders not in the cache cannot be translated (draws using them are skipped)"); return 0; }
    char line[4096]; snprintf(line, sizeof line, "%s --service --target spirv --out \"%s\" --chain vk1", cmd, cache_dir());
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, out_w;
    if (!CreatePipe(&in_r, &g_in_w, &sa, 0) || !CreatePipe(&g_out_r, &out_w, &sa, 0)) return 0;
    SetHandleInformation(g_in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { VKLOG("cannot start the shader translator: %s", line); return 0; }
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    VKLOG("shader translator: %s", line);
    return g_svc = 1;
}
static int svc_call(const char *req, char *reply, size_t n) {
    if (!svc_start()) return 0;
    DWORD w; size_t len = strlen(req);
    if (!WriteFile(g_in_w, req, (DWORD)len, &w, NULL) || !WriteFile(g_in_w, "\n", 1, &w, NULL)) return 0;
    size_t k = 0; char c; DWORD r;
    while (k + 1 < n && ReadFile(g_out_r, &c, 1, &r, NULL) && r == 1) { if (c == '\n') break; reply[k++] = c; }
    reply[k] = 0;
    return k > 0;
}
#else
static int svc_call(const char *req, char *reply, size_t n) { (void)req; (void)reply; (void)n; return 0; }
#endif

static inline uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }

/* answers: JSON object text {"tex": {...}, "img": {...}, "attr": {...}, "fout": {...}}; returns 1 with path_base set */
int vk_rt_variant(int stage, const uint8_t *control, const uint8_t *code, const char *answers, char *path_base, size_t n) {
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    if (!c1t || c1t > 16u << 20 || c1o > c1t || (stage != 5 && c1o < 0x30)) { VKONCE("shader with an implausible code area (%#x / %#x)", c1t, c1o); return 0; }
    uint64_t hs = vk_hash(control, ctl_key_len(control), 0); hs = vk_hash(code, c1t, hs);
    uint64_t ha = vk_hash(answers, strlen(answers), 0x9e3779b97f4a7c15ull);
    char variant[64]; snprintf(variant, sizeof variant, "%016llx-%012llx", (unsigned long long)hs, (unsigned long long)(ha & 0xffffffffffffull));
    pthread_mutex_lock(&g_rt_mu);
    snprintf(path_base, n, "%s/%s_v", cache_dir(), variant);
    char probe[1100]; snprintf(probe, sizeof probe, "%s.json", path_base);
    if (file_exists(probe)) { pthread_mutex_unlock(&g_rt_mu); return 1; }
    const char *sn = stage == 0 ? "vs" : stage == 1 ? "ps" : "cs";
    char code_path[1100]; snprintf(code_path, sizeof code_path, "%s/%s.code", cache_dir(), variant);
    size_t skip = stage == 5 ? 0 : 0x30;
    FILE *f = fopen(code_path, "wb"); if (!f) { pthread_mutex_unlock(&g_rt_mu); return 0; }
    fwrite(code + skip, 1, c1t - skip, f); fclose(f);
    for (char *p = code_path; *p; p++) if (*p == '\\') *p = '/';
    size_t c1n = c1t - c1o; char *c1h = malloc(2 * c1n + 1); hex(c1h, code + c1o, c1n);
    char local[64] = "";
    if (stage == 5) { uint32_t l[3]; memcpy(l, control + 0x748, 12); snprintf(local, sizeof local, ", \"local\": [%u, %u, %u]", l[0], l[1], l[2]); }
    int dmw = totk_depth_near_minus_w ? totk_depth_near_minus_w() : 0;
    size_t rl = strlen(answers) + 2 * c1n + 2048; char *req = malloc(rl);
    // answers is an object: splice its members into the request
    const char *ab = strchr(answers, '{'); const char *ae = strrchr(answers, '}');
    snprintf(req, rl, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": \"%s\", \"c1\": \"%s\"%s, \"depth_minus_w\": %s, \"caps\": %s, %.*s}",
             variant, sn, code_path, c1h, local, dmw ? "true" : "false", nvn_vk_translator_caps(), ab && ae > ab ? (int)(ae - ab - 1) : 0, ab ? ab + 1 : "");
    free(c1h);
    char reply[2048]; int ok = svc_call(req, reply, sizeof reply);
    free(req);
    if (!ok || !strstr(reply, "\"ok\": true")) { VKLOG("[shader] translation failed for %s %s: %s", sn, variant, ok ? reply : "no translator"); pthread_mutex_unlock(&g_rt_mu); return 0; }
    g_translated++;
    if (g_translated <= 8 || g_translated % 100 == 0) VKLOG("[shader] translated %llu: %s %s", (unsigned long long)g_translated, sn, variant);
    pthread_mutex_unlock(&g_rt_mu);
    return 1;
}
