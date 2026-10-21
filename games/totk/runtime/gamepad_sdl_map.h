/* SDL3 gamepad buttons -> the physical button bits of controller_map.h (C9). SDL3 names the face buttons by position
   (SOUTH / EAST / WEST / NORTH) on every controller, like controller_map.h does; SDL_GetGamepadButtonLabel says what is
   printed on them. gamepad_layout.h decides per controller whether the game's Switch A / B / X / Y follow position
   (PlayStation, Xbox: default) or label (Nintendo controllers, or TOTK_FACE_BUTTONS=label); on a Nintendo controller the
   label A is on the east button, so both give the same result there, as on the Mac.
   Header-only (SDL enums, no SDL calls): runtime/gamepad_sdl.c and tests/input_win/test_gamepad_map.c. */
#pragma once
#include <stdint.h>
#include <SDL3/SDL_gamepad.h>
#include "gamepad_layout.h"

/* Analog trigger past this (0..32767) counts as pressed (l2 / r2). */
enum { GAMEPAD_SDL_TRIGGER_PRESSED = 8192 };

/* The physical bit (controller_physical_names index) of a non-face SDL button, -1 for none. */
static inline int gamepad_sdl_physical_bit(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0;
    case SDL_GAMEPAD_BUTTON_EAST: return 1;
    case SDL_GAMEPAD_BUTTON_WEST: return 2;
    case SDL_GAMEPAD_BUTTON_NORTH: return 3;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 4;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 5;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 6;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 7;
    case SDL_GAMEPAD_BUTTON_START: return 10;           /* menu: plus */
    case SDL_GAMEPAD_BUTTON_BACK: return 11;            /* view: minus */
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 12;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 13;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 14;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 15;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return 16;
    default: return -1;                                 /* guide, misc, paddles: unused, as on the Mac */
    }
}

/* Which face label an SDL label is, as GameController's buttonA / B / X / Y (0..3), -1 for unknown.
   PlayStation: Cross = A, Circle = B, Square = X, Triangle = Y (GameController's own naming). */
static inline int gamepad_sdl_label_index(int label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 0;
    case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return 1;
    case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return 2;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return 3;
    default: return -1;
    }
}

/* Physical button bits of one controller.
   down[b]: SDL button b held (SDL_GAMEPAD_BUTTON_COUNT entries); labels[0..3]: SDL_GetGamepadButtonLabel of SOUTH, EAST,
   WEST, NORTH; lt / rt: trigger axes 0..32767; by_position: gamepad_face_by_position for this controller. */
static inline uint64_t gamepad_sdl_physical(const uint8_t *down, const int labels[4], int lt, int rt, int by_position) {
    uint64_t bits = 0;
    if (by_position) {
        for (int b = 0; b < 4; b++) if (down[b]) bits |= 1ULL << b;   /* SOUTH..NORTH are bits 0..3 already */
    } else {
        int held[4] = {0, 0, 0, 0}, seen = 0;   /* by label: A, B, X, Y */
        for (int b = 0; b < 4; b++) { int k = gamepad_sdl_label_index(labels[b]); if (k >= 0) { held[k] |= down[b]; seen++; } }
        if (seen == 4) bits |= gamepad_face_positions(held[0], held[1], held[2], held[3], 0);
        else for (int b = 0; b < 4; b++) if (down[b]) bits |= 1ULL << b;   /* labels unknown: by position */
    }
    for (int b = 4; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        int bit = gamepad_sdl_physical_bit(b);
        if (bit >= 0 && down[b]) bits |= 1ULL << bit;
    }
    if (lt > GAMEPAD_SDL_TRIGGER_PRESSED) bits |= 1ULL << 8;
    if (rt > GAMEPAD_SDL_TRIGGER_PRESSED) bits |= 1ULL << 9;
    return bits;
}
