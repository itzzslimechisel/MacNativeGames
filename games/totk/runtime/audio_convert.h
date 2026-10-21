/* Sample format conversion from the mixer's interleaved float32 stereo to a device format (C8, used by
   runtime/audio_out_wasapi.c when the device refuses the float32 stereo stream and only its own mix format is left).
   Pure C, no host APIs: tests/audio_out/test_audio_convert.c covers it.
   Channels: 1 = the average of left and right; 2+ = left and right in the first two channels (front left / front right
   in every WAVEFORMATEXTENSIBLE channel mask Windows uses), the others silent. Integer formats clamp to [-1, 1]. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    AUDIO_FMT_F32 = 0,      /* float32 */
    AUDIO_FMT_S16,          /* int16 */
    AUDIO_FMT_S24,          /* int24 packed, 3 bytes little endian */
    AUDIO_FMT_S24_32,       /* int24 in the high 24 bits of an int32 container (WAVEFORMATEXTENSIBLE 32/24) */
    AUDIO_FMT_S32,          /* int32 */
} AudioSampleFormat;

static inline int audio_fmt_bytes(AudioSampleFormat f) {
    return f == AUDIO_FMT_S16 ? 2 : f == AUDIO_FMT_S24 ? 3 : 4;
}

/* Maps a WAVEFORMAT description (float or not, container bits, valid bits) to a format; -1 = not supported. */
static inline int audio_fmt_from_wave(int is_float, int container_bits, int valid_bits) {
    if (valid_bits <= 0) valid_bits = container_bits;
    if (is_float) return container_bits == 32 ? AUDIO_FMT_F32 : -1;
    if (container_bits == 16) return AUDIO_FMT_S16;
    if (container_bits == 24) return AUDIO_FMT_S24;
    if (container_bits == 32) return valid_bits == 24 ? AUDIO_FMT_S24_32 : AUDIO_FMT_S32;
    return -1;
}

static inline float audio_clampf(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v != v ? 0.0f : v; }

/* One sample to the device format at dst (little endian). */
static inline void audio_put_sample(uint8_t *dst, AudioSampleFormat f, float v) {
    switch (f) {
    case AUDIO_FMT_F32: memcpy(dst, &v, 4); break;
    case AUDIO_FMT_S16: { int16_t s = (int16_t)lrintf(audio_clampf(v) * 32767.0f); dst[0] = (uint8_t)s; dst[1] = (uint8_t)((uint16_t)s >> 8); break; }
    case AUDIO_FMT_S24: { int32_t s = (int32_t)lrintf(audio_clampf(v) * 8388607.0f); dst[0] = (uint8_t)s; dst[1] = (uint8_t)(s >> 8); dst[2] = (uint8_t)(s >> 16); break; }
    case AUDIO_FMT_S24_32: { int32_t s = (int32_t)lrintf(audio_clampf(v) * 8388607.0f) * 256; memcpy(dst, &s, 4); break; }
    case AUDIO_FMT_S32: { double d = (double)audio_clampf(v) * 2147483647.0; int32_t s = (int32_t)llrint(d); memcpy(dst, &s, 4); break; }
    }
}

/* frames of interleaved stereo float `in` -> `out` in format f with `channels` channels (frames * channels samples). */
static inline void audio_convert_stereo(const float *in, uint32_t frames, void *out, AudioSampleFormat f, int channels) {
    uint8_t *o = out; int b = audio_fmt_bytes(f);
    if (f == AUDIO_FMT_F32 && channels == 2) { memcpy(out, in, (size_t)frames * 2 * sizeof(float)); return; }
    for (uint32_t i = 0; i < frames; i++) {
        float l = in[2 * i], r = in[2 * i + 1];
        if (channels == 1) { audio_put_sample(o, f, 0.5f * (l + r)); o += b; continue; }
        audio_put_sample(o, f, l); o += b;
        audio_put_sample(o, f, r); o += b;
        for (int c = 2; c < channels; c++) { audio_put_sample(o, f, 0.0f); o += b; }
    }
}
