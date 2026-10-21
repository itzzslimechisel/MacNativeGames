/* Internal seam between runtime/frontend_win.c (window, event loop: the SDL main thread) and the Windows controller
   code (runtime/gamepad_sdl.c, runtime/gamepad_feedback_sdl.c). Everything here runs on the SDL main thread unless it
   says otherwise. The public surface stays the Mac one in runtime.h (runtime_gamepad_*, runtime_rumble_set, ...). */
#pragma once
#include <stdint.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

/* runtime/frontend_win.c: initializes SDL video and creates the game window without starting the game thread or the
   event loop (runtime_front_run does both; probes and tests call this alone). 1 on success. */
int front_window_create(int hidden);

int gamepad_sdl_event(const SDL_Event *e);          /* 1: a gamepad / sensor event, handled */
void gamepad_sdl_tick(uint64_t now_ns);              /* every main-loop pass (<= 10 ms): tap pulses, mouse stick, rumble */
/* Optional mouse (TOTK_MOUSE=1): relative motion drives the right stick, left / right / middle button the physical
   r2 / l2 / r3 buttons (ZR / ZL / right stick click by default, remappable in controls.txt). */
int gamepad_sdl_mouse_enabled(void);
void gamepad_sdl_mouse_motion(float dx, float dy);
void gamepad_sdl_mouse_button(int sdl_button, int down);

/* runtime/gamepad_feedback_sdl.c (the Mac's runtime_feedback_* take a GCController *). */
void runtime_feedback_attach(SDL_Gamepad *pad);
void runtime_feedback_detach(SDL_Gamepad *pad);
void runtime_feedback_quiet(void);                   /* any thread */
void gamepad_feedback_tick(uint64_t now_ns);
void gamepad_feedback_sensor(const SDL_GamepadSensorEvent *e);
