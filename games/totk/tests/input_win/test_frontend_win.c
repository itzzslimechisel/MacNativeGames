/* runtime/frontend_win.c + gamepad_sdl.c + gamepad_feedback_sdl.c linked against stand-ins for the rest of the runtime:
   the hidden game window (Vulkan-capable when a loader is installed), its drawable size and HWND, the Vulkan instance
   extensions, the VI overrides, controller start-up and the runtime_gamepad_read contract (presses latched until read),
   rumble values with no controller. Exit 77 (skipped) where no window can be created (no desktop session). */
#include "../platform/test_common.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "frontend_sdl.h"
#include "../third_party/d2r_gfx/host/front.h"

/* ---- stand-ins for the runtime ---- */
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(1); }
uint64_t runtime_now_ns(void) { return SDL_GetTicksNS(); }
void runtime_input_key(unsigned code, int down) { (void)code; (void)down; }
void runtime_input_clear(void) { runtime_gamepad_clear(); }
void runtime_oe_push_message(uint32_t message) { (void)message; }
int runtime_fs_save_busy(void) { return 0; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
void hle_signal_system_event(void *event) { (void)event; }
int fps_overlay_toggle_hints(void) { return 0; }
int runtime_gyro_enabled(void) { return 0; }
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns) { (void)acc_g; (void)gyro_rad_s; (void)now_ns; }
void runtime_motion_live(int on) { (void)on; }
void runtime_motion_deadzone_default(double deg_s) { (void)deg_s; }

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!front_window_create(1)) { printf("frontend_win: no window possible here, skipped\n"); return 77; }
    REQUIRE(front_sdl_window() != NULL);
    CHECK(front_win32_hwnd() != NULL && IsWindow((HWND)front_win32_hwnd()));
    uint32_t w = 0, h = 0;
    CHECK(front_drawable_size(&w, &h) && w >= 320 && h >= 180);
    printf("frontend_win: drawable %ux%u\n", w, h);
    uint32_t n = 0; const char *const *ext = front_vk_instance_extensions(&n);
    if (ext) {
        int surface = 0;
        for (uint32_t i = 0; i < n; i++) { printf("frontend_win: Vulkan instance extension %s\n", ext[i]); if (!strcmp(ext[i], "VK_KHR_surface")) surface = 1; }
        CHECK(surface);
    } else printf("frontend_win: no Vulkan loader: plain window (backend will report it)\n");
    CHECK(front_vk_surface(NULL) == 0);
    CHECK(front_metal_layer() == NULL);
    CHECK((front_game_res(1) & 0xffffffff) == 1280);

    /* VI: the game's display path */
    CHECK(runtime_front_override("_ZN2nn2vi10InitializeEv") != NULL);
    CHECK(runtime_front_override("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE") != NULL);
    CHECK(runtime_front_override("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE") != NULL);
    CHECK(runtime_front_override("nothing") == NULL);

    /* Controllers: start-up works without any controller; a short touch press is latched until one read. */
    runtime_gamepad_start();
    RuntimeGamepadState s; runtime_gamepad_read(&s);
    printf("frontend_win: controllers connected: %d\n", s.connected);
    RuntimeGamepadState touch = {.buttons = 1, .lx = 1000}; runtime_touch_pad_set(&touch);
    RuntimeGamepadState none = {0}; runtime_touch_pad_set(&none);
    runtime_gamepad_read(&s); CHECK(s.buttons & 1);            /* pressed and released before the read: still seen once */
    runtime_gamepad_read(&s); CHECK(!(s.buttons & 1));
    runtime_rumble_set(0, 0.5f, 160, 0.5f, 320); runtime_rumble_set(7, 1, 1, 1, 1);
    for (int i = 0; i < 5; i++) { SDL_Event e; while (SDL_PollEvent(&e)) gamepad_sdl_event(&e); gamepad_sdl_tick(runtime_now_ns()); SDL_Delay(10); }
    runtime_feedback_quiet();
    runtime_gamepad_clear();

    /* Boot status lines must be safe from any thread before / after the first frame. */
    runtime_boot_progress("stage", 1, 2); runtime_boot_status("stage", "detail"); runtime_warm_progress(1, 2, 1, 2, 0, 1);
    runtime_warm_done(); front_frame_presented();
    CHECK(runtime_front_focus() == 1);
    TEST_DONE("frontend_win");
}
