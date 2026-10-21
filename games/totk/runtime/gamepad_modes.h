/* Controller layout modes (controller_map.h: hold, tap, toggle, aim) applied to the physical buttons held, and the stick
   deadzone: the same rules runtime/gamepad.m applies on the Mac, as plain C state so runtime/gamepad_sdl.c (Windows)
   shares them and tests/input_win/test_gamepad_map.c can check them without a controller. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "controller_map.h"

typedef struct {
    uint16_t map[CONTROLLER_BUTTONS];
    uint8_t modes[CONTROLLER_BUTTONS];
    uint64_t physical_prev;
    uint64_t tap_until[CONTROLLER_BUTTONS];   /* ns */
    int toggle_on[CONTROLLER_BUTTONS], toggle_moved[CONTROLLER_BUTTONS];
} GamepadModes;

static inline void gamepad_modes_reset(GamepadModes *g) {
    g->physical_prev = 0;
    memset(g->tap_until, 0, sizeof g->tap_until); memset(g->toggle_on, 0, sizeof g->toggle_on); memset(g->toggle_moved, 0, sizeof g->toggle_moved);
}
static inline void gamepad_modes_init(GamepadModes *g) { controller_map_default(g->map, g->modes); gamepad_modes_reset(g); }

/* The Switch buttons for the physical buttons held now (bit i = controller_physical_names[i]). moving: the left stick is
   off center; connected: a controller is attached. *wake (may be NULL) gets the earliest time a tap pulse ends after
   now, 0 for none: call again then, even without a controller event, so the pulse is released. */
static inline uint64_t gamepad_modes_apply(GamepadModes *g, uint64_t physical, int moving, int connected, uint64_t now, uint64_t *wake) {
    uint64_t edges = physical & ~g->physical_prev, synthetic = 0, suppressed = 0, next_wake = 0;
    g->physical_prev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(g->modes[i]);
        if (kind == CONTROLLER_TAP && (edges & bit)) g->tap_until[i] = now + 400000000ULL;
        if (kind == CONTROLLER_TOGGLE) {
            if (edges & bit) { g->toggle_on[i] = !g->toggle_on[i]; g->toggle_moved[i] = moving; }
            if (g->toggle_on[i] && moving) g->toggle_moved[i] = 1;
            if (g->toggle_on[i] && ((g->toggle_moved[i] && !moving) || !connected)) g->toggle_on[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            uint64_t fire = 1ULL << controller_mode_fire(g->modes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= g->map[i];
        }
        int active = kind == CONTROLLER_TAP ? now < g->tap_until[i] : kind == CONTROLLER_TOGGLE ? g->toggle_on[i] : 0;
        if (active) synthetic |= g->map[i];
        if (kind == CONTROLLER_TAP && now < g->tap_until[i] && (!next_wake || g->tap_until[i] < next_wake)) next_wake = g->tap_until[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) if (g->modes[i] != CONTROLLER_HOLD) held &= ~(1ULL << i);
    if (wake) *wake = next_wake;
    return controller_map_apply(g->map, held) | synthetic;
}

/* -1..1 -> the Switch's -32767..32767 with gamepad.m's per-axis 0.12 deadzone (the rest rescaled to full range). */
static inline int32_t gamepad_stick_axis(float value) {
    const float deadzone = 0.12f;
    float magnitude = fabsf(value);
    if (!(magnitude > deadzone)) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}
static inline int32_t gamepad_stronger_axis(int32_t a, int32_t b) { return abs(b) > abs(a) ? b : a; }

/* Rumble strength of one side from the game's value {amp_low, freq_low, amp_high, freq_high} (gamepad_feedback.m
   intensity_of): the bands' combined amplitude times gain, 0..1. */
static inline float gamepad_rumble_intensity(const float v[4], float gain) {
    float a = sqrtf(v[0] * v[0] + v[2] * v[2]) * gain;
    return a > 0 ? (a < 1 ? a : 1) : 0;
}
