/* runtime/keymap_sdl.h: Windows keys (USB HID usages = SDL3 scancodes) reach runtime/input.c as the Mac key codes of the
   keys in the same place, so every Switch button the Mac keyboard has is on the same key on Windows. Headless. */
#include "../platform/test_common.h"
#include "keymap_sdl.h"

/* HID usages of the keys runtime/input.c gives a meaning to (button() and the stick keys), with the Mac code each must
   give: J A, K / Space B, I X, U Y, Z / C stick clicks, Q L, E R, Option ZL, Shift ZR, Return Plus, Tab / Esc Minus,
   F T H G D-pad, W A S D left stick, arrows right stick, 1-5 shortcuts. */
static const struct { unsigned hid, mac; const char *what; } expect[] = {
    {13, 38, "J: A"}, {14, 40, "K: B"}, {44, 49, "Space: B"}, {12, 34, "I: X"}, {24, 32, "U: Y"},
    {29, 6, "Z: left stick click"}, {6, 8, "C: right stick click"}, {20, 12, "Q: L"}, {8, 14, "E: R"},
    {226, 58, "Left Alt: ZL"}, {225, 56, "Left Shift: ZR"}, {40, 36, "Return: Plus"}, {43, 48, "Tab: Minus"}, {41, 53, "Esc: Minus"},
    {9, 3, "F: D-pad left"}, {23, 17, "T: D-pad up"}, {11, 4, "H: D-pad right"}, {10, 5, "G: D-pad down"},
    {26, 13, "W: left stick up"}, {4, 0, "A: left stick left"}, {22, 1, "S: left stick down"}, {7, 2, "D: left stick right"},
    {82, 126, "Up: right stick up"}, {81, 125, "Down: right stick down"}, {80, 123, "Left: right stick left"}, {79, 124, "Right: right stick right"},
    {30, 18, "1: overlays"}, {31, 19, "2: interpolation"}, {32, 20, "3: picture"}, {33, 21, "4: ultrawide"}, {34, 23, "5: TAA"},
};

int main(void) {
    for (unsigned i = 0; i < sizeof expect / sizeof *expect; i++) {
        unsigned got = keymap_mac_from_hid(expect[i].hid);
        if (got != expect[i].mac) fprintf(stderr, "  %s: HID %u -> %u, want %u\n", expect[i].what, expect[i].hid, got, expect[i].mac);
        CHECK(got == expect[i].mac);
    }
    /* No two keys share a Mac code; every code fits runtime/input.c's 128-entry key table. */
    int seen[128] = {0};
    for (unsigned hid = 0; hid < 256; hid++) {
        unsigned m = keymap_mac_from_hid(hid);
        if (m == KEYMAP_NONE) continue;
        CHECK(m < 128);
        if (m < 128) { if (seen[m]) fprintf(stderr, "  Mac code %u from HID %u and %d\n", m, hid, seen[m] - 1); CHECK(!seen[m]); seen[m] = (int)hid + 1; }
    }
    /* Modifiers the Mac frontend never forwards stay unmapped (Ctrl, Win, Caps, right Shift / Alt). */
    CHECK(keymap_mac_from_hid(224) == KEYMAP_NONE && keymap_mac_from_hid(227) == KEYMAP_NONE && keymap_mac_from_hid(57) == KEYMAP_NONE);
    CHECK(keymap_mac_from_hid(229) == KEYMAP_NONE && keymap_mac_from_hid(230) == KEYMAP_NONE);
    CHECK(keymap_mac_from_hid(0) == KEYMAP_NONE && keymap_mac_from_hid(1000) == KEYMAP_NONE);
    TEST_DONE("keymap");
}
