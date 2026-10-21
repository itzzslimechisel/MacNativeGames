/* Null audio output (docs/windows-port-plan.md C8 seam, runtime/audio_out.h): headless bring-up. A host thread pulls
   from the mixer at the device rate and drops the samples, so the renderer's timing behaves as with a real device. */
#include "audio_out.h"
#include "host_thread.h"
#include "host_time.h"
#include <stdatomic.h>
#include <stdlib.h>

struct AudioOut { int rate, channels; audio_out_pull_fn pull; void *ctx; host_thread *thread; _Atomic int run; };
enum { PERIOD = 240 };   /* 5 ms at 48 kHz */

static void *pump(void *arg) {
    AudioOut *o = arg;
    float *buf = calloc((size_t)PERIOD * o->channels, sizeof(float));
    uint64_t next = host_now_ns(), step = 1000000000ull * PERIOD / (uint64_t)o->rate;
    while (atomic_load(&o->run)) {
        if (buf) o->pull(o->ctx, buf, PERIOD);
        next += step;
        uint64_t now = host_now_ns();
        if (next > now) host_sleep_ns(next - now); else next = now;
    }
    free(buf);
    return NULL;
}
AudioOut *audio_out_open(int rate, int channels, audio_out_pull_fn pull, void *ctx) {
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate > 0 ? rate : 48000; o->channels = channels > 0 ? channels : 2; o->pull = pull; o->ctx = ctx;
    atomic_store(&o->run, 1);
    if (host_thread_create(&o->thread, pump, o, 0)) { free(o); return NULL; }
    return o;
}
int audio_out_restart(AudioOut *out, int recreate) { (void)recreate; return out != NULL; }
void audio_out_close(AudioOut *out) {
    if (!out) return;
    atomic_store(&out->run, 0);
    host_thread_join(out->thread, NULL);
    free(out);
}
const char *audio_out_backend(void) { return "null"; }
