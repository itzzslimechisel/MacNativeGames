/* runtime/gamepad_sdl_map.h + runtime/gamepad_modes.h: SDL3 gamepad buttons to Switch buttons as the Mac does them
   (face buttons by position or label, the other buttons, triggers, layout modes, stick deadzone, rumble strength).
   Headless: SDL enums only, no SDL calls. */
#include "../platform/test_common.h"
#include "gamepad_sdl_map.h"
#include "gamepad_modes.h"

enum { SW_A = 1, SW_B = 2, SW_X = 4, SW_Y = 8, SW_LS = 16, SW_RS = 32, SW_L = 64, SW_R = 128, SW_ZL = 256, SW_ZR = 512,
       SW_PLUS = 1024, SW_MINUS = 2048, SW_LEFT = 4096, SW_UP = 8192, SW_RIGHT = 16384, SW_DOWN = 32768 };

static const int XBOX[4] = {SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y};
static const int PS[4] = {SDL_GAMEPAD_BUTTON_LABEL_CROSS, SDL_GAMEPAD_BUTTON_LABEL_CIRCLE, SDL_GAMEPAD_BUTTON_LABEL_SQUARE, SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE};
/* Nintendo: south B, east A, west Y, north X (SDL3 reports positions, labels say what is printed) */
static const int NIN[4] = {SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_X};
static const int UNKNOWN[4] = {SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN};

/* Switch buttons for one SDL button held, default layout. */
static uint64_t press(int button, const int labels[4], int by_position) {
    uint8_t down[SDL_GAMEPAD_BUTTON_COUNT] = {0}; down[button] = 1;
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(map, modes);
    return controller_map_apply(map, gamepad_sdl_physical(down, labels, 0, 0, by_position));
}

int main(void) {
    /* By position (Xbox / PlayStation default): the Switch's own layout, A on the right. */
    CHECK(press(SDL_GAMEPAD_BUTTON_EAST, XBOX, 1) == SW_A);
    CHECK(press(SDL_GAMEPAD_BUTTON_SOUTH, XBOX, 1) == SW_B);
    CHECK(press(SDL_GAMEPAD_BUTTON_NORTH, XBOX, 1) == SW_X);
    CHECK(press(SDL_GAMEPAD_BUTTON_WEST, XBOX, 1) == SW_Y);
    CHECK(press(SDL_GAMEPAD_BUTTON_EAST, PS, 1) == SW_A);   /* Circle */
    /* By label (TOTK_FACE_BUTTONS=label): the printed letter. */
    CHECK(press(SDL_GAMEPAD_BUTTON_SOUTH, XBOX, 0) == SW_A);
    CHECK(press(SDL_GAMEPAD_BUTTON_EAST, XBOX, 0) == SW_B);
    CHECK(press(SDL_GAMEPAD_BUTTON_WEST, XBOX, 0) == SW_X);
    CHECK(press(SDL_GAMEPAD_BUTTON_NORTH, XBOX, 0) == SW_Y);
    CHECK(press(SDL_GAMEPAD_BUTTON_SOUTH, PS, 0) == SW_A);   /* Cross = GameController's buttonA */
    CHECK(press(SDL_GAMEPAD_BUTTON_NORTH, PS, 0) == SW_Y);   /* Triangle */
    /* Nintendo controllers: label and position agree (A is the east button). */
    for (int by = 0; by < 2; by++) {
        CHECK(press(SDL_GAMEPAD_BUTTON_EAST, NIN, by) == SW_A);
        CHECK(press(SDL_GAMEPAD_BUTTON_SOUTH, NIN, by) == SW_B);
        CHECK(press(SDL_GAMEPAD_BUTTON_NORTH, NIN, by) == SW_X);
        CHECK(press(SDL_GAMEPAD_BUTTON_WEST, NIN, by) == SW_Y);
    }
    CHECK(press(SDL_GAMEPAD_BUTTON_EAST, UNKNOWN, 0) == SW_A);   /* no labels: by position */
    /* gamepad_layout.h's choice from SDL's type string / name */
    CHECK(gamepad_face_by_position("xbox360", "Xbox 360 Controller") == 1);
    CHECK(gamepad_face_by_position("ps5", "DualSense Wireless Controller") == 1);
    CHECK(gamepad_face_by_position("switchpro", "Pro Controller") == 0);
    CHECK(gamepad_face_by_position("joyconpair", "Joy-Con (L/R)") == 0);

    /* The other buttons: FullKey bit i for physical i. */
    CHECK(press(SDL_GAMEPAD_BUTTON_LEFT_STICK, XBOX, 1) == SW_LS);
    CHECK(press(SDL_GAMEPAD_BUTTON_RIGHT_STICK, XBOX, 1) == SW_RS);
    CHECK(press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XBOX, 1) == SW_L);
    CHECK(press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XBOX, 1) == SW_R);
    CHECK(press(SDL_GAMEPAD_BUTTON_START, XBOX, 1) == SW_PLUS);
    CHECK(press(SDL_GAMEPAD_BUTTON_BACK, XBOX, 1) == SW_MINUS);
    CHECK(press(SDL_GAMEPAD_BUTTON_DPAD_LEFT, XBOX, 1) == SW_LEFT);
    CHECK(press(SDL_GAMEPAD_BUTTON_DPAD_UP, XBOX, 1) == SW_UP);
    CHECK(press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XBOX, 1) == SW_RIGHT);
    CHECK(press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, XBOX, 1) == SW_DOWN);
    CHECK(press(SDL_GAMEPAD_BUTTON_TOUCHPAD, PS, 1) == 0);   /* physical 16: nothing by default */
    CHECK(press(SDL_GAMEPAD_BUTTON_GUIDE, XBOX, 1) == 0);
    { uint8_t down[SDL_GAMEPAD_BUTTON_COUNT] = {0}; down[SDL_GAMEPAD_BUTTON_TOUCHPAD] = 1;
      CHECK(gamepad_sdl_physical(down, PS, 0, 0, 1) == 1ULL << 16); }
    /* Triggers: ZL / ZR past a quarter. */
    { uint8_t down[SDL_GAMEPAD_BUTTON_COUNT] = {0};
      CHECK(gamepad_sdl_physical(down, XBOX, 30000, 0, 1) == 1ULL << 8);
      CHECK(gamepad_sdl_physical(down, XBOX, 0, 30000, 1) == 1ULL << 9);
      CHECK(gamepad_sdl_physical(down, XBOX, 4000, 4000, 1) == 0); }

    /* Layout modes (controls.txt), driven like gamepad_sdl.c drives them. */
    GamepadModes g; gamepad_modes_init(&g);
    const char *err = NULL;
    REQUIRE(controller_map_parse("north = x tap\nl3 = lstick toggle\nl2 = zr aim r2\n", g.map, g.modes, &err) == 0);
    uint64_t wake = 0, s;
    const uint64_t N = 1ULL << 3, L3 = 1ULL << 4, L2 = 1ULL << 8, R2 = 1ULL << 9, T0 = 1000000000ULL, T1 = T0 + 1000000000ULL;   /* T1: after the tap pulse */
    s = gamepad_modes_apply(&g, N, 0, 1, T0, &wake); CHECK(s == SW_X && wake == T0 + 400000000ULL);   /* tap: 400 ms pulse */
    s = gamepad_modes_apply(&g, 0, 0, 1, T0 + 100000000ULL, &wake); CHECK(s == SW_X);                /* released, still on */
    s = gamepad_modes_apply(&g, 0, 0, 1, T0 + 400000000ULL, &wake); CHECK(s == 0 && wake == 0);      /* pulse over */
    s = gamepad_modes_apply(&g, L3, 0, 1, T1, NULL); CHECK(s == SW_LS);                              /* toggle on at rest */
    s = gamepad_modes_apply(&g, 0, 0, 1, T1, NULL); CHECK(s == SW_LS);                               /* waits for a move */
    s = gamepad_modes_apply(&g, 0, 1, 1, T1, NULL); CHECK(s == SW_LS);                               /* running */
    s = gamepad_modes_apply(&g, 0, 0, 1, T1, NULL); CHECK(s == 0);                                   /* stick back: off */
    s = gamepad_modes_apply(&g, L2, 0, 1, T1, NULL); CHECK(s == SW_ZR);                              /* aim: draw */
    s = gamepad_modes_apply(&g, L2 | R2, 0, 1, T1, NULL); CHECK(s == 0);                             /* fire: release, r2 itself suppressed */
    s = gamepad_modes_apply(&g, R2, 0, 1, T1, NULL); CHECK(s == SW_ZR);                              /* r2 alone: its own line (zr) */
    gamepad_modes_reset(&g);
    s = gamepad_modes_apply(&g, 1ULL << 1, 0, 1, T1, NULL); CHECK(s == SW_A);                        /* hold: east -> A */

    /* Stick deadzone and scale (gamepad.m stick_axis). */
    CHECK(gamepad_stick_axis(0.0f) == 0 && gamepad_stick_axis(0.12f) == 0 && gamepad_stick_axis(-0.1f) == 0);
    CHECK(gamepad_stick_axis(1.0f) == 32767 && gamepad_stick_axis(-1.0f) == -32767 && gamepad_stick_axis(1.5f) == 32767);
    CHECK(gamepad_stick_axis(0.56f) > 16000 && gamepad_stick_axis(0.56f) < 16767);
    CHECK(gamepad_stronger_axis(100, -200) == -200 && gamepad_stronger_axis(300, -200) == 300);

    /* Rumble strength (gamepad_feedback.m intensity_of). */
    { const float quiet[4] = {0, 160, 0, 320}, half[4] = {0.3f, 160, 0.4f, 320}, loud[4] = {1, 160, 1, 320};
      CHECK(gamepad_rumble_intensity(quiet, 1) == 0);
      CHECK(gamepad_rumble_intensity(half, 1) > 0.499f && gamepad_rumble_intensity(half, 1) < 0.501f);
      CHECK(gamepad_rumble_intensity(half, 4) == 1);
      CHECK(gamepad_rumble_intensity(loud, 1) == 1); }

    TEST_DONE("gamepad_map");
}
