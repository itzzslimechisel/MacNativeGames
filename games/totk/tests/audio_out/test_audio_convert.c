/* runtime/audio_convert.h: the mixer's float32 stereo to the device formats WASAPI may insist on. Headless. */
#include "../platform/test_common.h"
#include "audio_convert.h"
#include <stdint.h>
#include <string.h>

static int32_t rd24(const uint8_t *p) { int32_t v = p[0] | p[1] << 8 | p[2] << 16; return v & 0x800000 ? v - 0x1000000 : v; }

int main(void) {
    const float in[8] = {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -3.0f, 0.25f, -0.25f};   /* 4 stereo frames; frame 2 out of range */

    /* format table */
    CHECK(audio_fmt_from_wave(1, 32, 32) == AUDIO_FMT_F32);
    CHECK(audio_fmt_from_wave(1, 64, 64) == -1);
    CHECK(audio_fmt_from_wave(0, 16, 16) == AUDIO_FMT_S16);
    CHECK(audio_fmt_from_wave(0, 24, 24) == AUDIO_FMT_S24);
    CHECK(audio_fmt_from_wave(0, 32, 24) == AUDIO_FMT_S24_32);
    CHECK(audio_fmt_from_wave(0, 32, 32) == AUDIO_FMT_S32);
    CHECK(audio_fmt_from_wave(0, 32, 0) == AUDIO_FMT_S32);
    CHECK(audio_fmt_from_wave(0, 8, 8) == -1);
    CHECK(audio_fmt_bytes(AUDIO_FMT_S16) == 2 && audio_fmt_bytes(AUDIO_FMT_S24) == 3 && audio_fmt_bytes(AUDIO_FMT_S24_32) == 4);

    /* float stereo: a straight copy */
    float f2[8]; audio_convert_stereo(in, 4, f2, AUDIO_FMT_F32, 2);
    CHECK(!memcmp(f2, in, sizeof f2));

    /* float 5.1: L, R, then silence */
    float f6[24]; memset(f6, 0x7f, sizeof f6); audio_convert_stereo(in, 4, f6, AUDIO_FMT_F32, 6);
    for (int i = 0; i < 4; i++) {
        CHECK(f6[6 * i] == in[2 * i] && f6[6 * i + 1] == in[2 * i + 1]);
        for (int c = 2; c < 6; c++) CHECK(f6[6 * i + c] == 0.0f);
    }

    /* mono: the average */
    float f1[4]; audio_convert_stereo(in, 4, f1, AUDIO_FMT_F32, 1);
    CHECK(f1[0] == 0.5f && f1[1] == -0.25f && f1[3] == 0.0f);

    /* int16, clamped */
    int16_t s16[8]; audio_convert_stereo(in, 4, s16, AUDIO_FMT_S16, 2);
    CHECK(s16[0] == 0 && s16[1] == 32767 && s16[2] == -32767 && s16[3] == 16384);
    CHECK(s16[4] == 32767 && s16[5] == -32767);   /* 2.0 / -3.0 clamp */
    CHECK(s16[6] == 8192 && s16[7] == -8192);

    /* int24 packed */
    uint8_t s24[4 * 2 * 3]; audio_convert_stereo(in, 4, s24, AUDIO_FMT_S24, 2);
    CHECK(rd24(s24 + 0) == 0 && rd24(s24 + 3) == 8388607 && rd24(s24 + 6) == -8388607);
    CHECK(rd24(s24 + 12) == 8388607 && rd24(s24 + 15) == -8388607);

    /* int24 in int32 containers: the value in the high 24 bits */
    int32_t s2432[8]; audio_convert_stereo(in, 4, s2432, AUDIO_FMT_S24_32, 2);
    CHECK(s2432[1] == 8388607 * 256 && s2432[2] == -8388607 * 256 && (s2432[3] & 0xff) == 0);

    /* int32 */
    int32_t s32[8]; audio_convert_stereo(in, 4, s32, AUDIO_FMT_S32, 2);
    CHECK(s32[0] == 0 && s32[1] == 2147483647 && s32[2] == -2147483647 && s32[5] == -2147483647);

    /* NaN becomes silence, not a full-scale click */
    float nan_in[2] = {0.0f / 0.0f, 0.0f}; int16_t nan_out[2];
    audio_convert_stereo(nan_in, 1, nan_out, AUDIO_FMT_S16, 2);
    CHECK(nan_out[0] == 0);

    TEST_DONE("audio_convert");
}
