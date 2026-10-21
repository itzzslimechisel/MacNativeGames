/* runtime/audio_out_wasapi.c against the real default output device: open at the mixer's format (48 kHz float32
   stereo), check the pull contract (sane request sizes, real-time rate), restart in place and rebuilt,
   close (no pull afterwards). Plays silence. Exit 77 (skipped) on a machine without an output device. */
#include "../platform/test_common.h"
#include "audio_out.h"
#include <windows.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic uint64_t frames_pulled, calls;
static _Atomic int bad, closed;

static void pull(void *ctx, float *out, uint32_t frames) {
    (void)ctx;
    if (atomic_load(&closed) || frames == 0 || frames > 48000) atomic_store(&bad, 1);
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    atomic_fetch_add(&frames_pulled, frames); atomic_fetch_add(&calls, 1);
}

static uint64_t run_for(int ms) { uint64_t a = atomic_load(&frames_pulled); Sleep(ms); return atomic_load(&frames_pulled) - a; }

int main(void) {
    CHECK(!strcmp(audio_out_backend(), "WASAPI"));
    CHECK(audio_out_open(0, 2, pull, NULL) == NULL);
    CHECK(audio_out_open(48000, 2, NULL, NULL) == NULL);
    AudioOut *o = audio_out_open(48000, 2, pull, NULL);
    if (!o) { printf("audio_out_wasapi: no output device, skipped\n"); return 77; }
    uint64_t n = run_for(500);
    printf("audio_out_wasapi: %llu frames in 0.5 s, %llu pulls\n", (unsigned long long)n, (unsigned long long)atomic_load(&calls));
    CHECK(n > 48000 / 4 && n < 48000);           /* ~24000 at real time */
    CHECK(audio_out_restart(o, 0) == 1);          /* running: nothing to do */
    CHECK(run_for(200) > 0);
    CHECK(audio_out_restart(o, 1) == 1);          /* rebuilt on the default device */
    CHECK(run_for(300) > 0);
    audio_out_close(o);
    atomic_store(&closed, 1);
    uint64_t after = run_for(100);
    CHECK(after == 0);
    CHECK(!atomic_load(&bad));
    audio_out_close(NULL);
    TEST_DONE("audio_out_wasapi");
}
