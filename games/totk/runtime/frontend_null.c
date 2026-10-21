/* Headless frontend (TOTK_HEADLESS bring-up, Windows x86-64): the runtime_front_* surface without a window. Boot
   progress goes to the log; nn::vi gets a display whose vsync events a host thread signals at 60 Hz, so the game's frame
   loop runs as with a window. */
#include "runtime.h"
#include "host_thread.h"
#include "host_time.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *);

void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    if (!total || done == total || !done) fprintf(stderr, "[totk] boot: %s %llu/%llu\n", stage, (unsigned long long)done, (unsigned long long)total);
}
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) { runtime_boot_progress(stage, done, total); }
void runtime_boot_status(const char *stage, const char *detail) { fprintf(stderr, "[totk] boot: %s: %s\n", stage, detail); }
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    (void)done; (void)total; (void)shaders; (void)shader_total; (void)pipelines; (void)pipeline_total;
}
void runtime_warm_done(void) {}
int runtime_front_focus(void) { return 1; }
int runtime_front_window_visible(void) { return 0; }
void runtime_front_request_close(void) {}
void runtime_shortcut_help_toggle(void) {}
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) { (void)req; if (out_size) out[0] = 0; return 0; }
void runtime_front_run(void (*callback)(void)) { callback(); }
void front_frame_presented(void) {}
uint64_t front_game_res(int docked) { (void)docked; return (720ULL << 32) | 1280; }

/* ---------- nn::vi ---------- */
static int viReady;
static unsigned displayIdentity, layerIdentity, windowIdentity;
static host_mutex vsyncLock = HOST_MUTEX_INIT;
static void *vsyncEvents[16];
static unsigned vsyncCount;
static host_thread *vsyncThread;
static void *vsync_pump(void *arg) {
    (void)arg;
    uint64_t next = host_now_ns();
    for (;;) {
        next += 16666667;
        uint64_t now = host_now_ns();
        if (next > now) host_sleep_ns(next - now); else next = now;
        void *ev[16]; unsigned n;
        host_mutex_lock(&vsyncLock); n = vsyncCount; memcpy(ev, vsyncEvents, sizeof ev); host_mutex_unlock(&vsyncLock);
        for (unsigned i = 0; i < n; i++) hle_signal_system_event(ev[i]);
    }
    return NULL;
}
static void vi_initialize(void) { viReady = 1; fprintf(stderr, "[totk] headless VI display initialized\n"); }
static uint32_t vi_open(void **out) { if (!viReady || !out) return 0xf601; *out = &displayIdentity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &displayIdentity) return 0xf601; *out = &layerIdentity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layerIdentity) return 0xf601; *out = &windowIdentity; return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &displayIdentity) return 0xf601;
    hle_bind_system_event(ev, 1);
    host_mutex_lock(&vsyncLock);
    int full = vsyncCount >= 16; if (!full) vsyncEvents[vsyncCount++] = ev;
    int start = !vsyncThread;
    host_mutex_unlock(&vsyncLock);
    if (full) runtime_fail("VI display event limit");
    if (start && host_thread_create(&vsyncThread, vsync_pump, NULL, 0)) runtime_fail("headless vsync thread");
    return 0;
}
void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
