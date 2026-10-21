/* audio_out.h on macOS / iOS: one output AudioUnit (DefaultOutput on the Mac, RemoteIO on iOS) pulling interleaved
   float32 through a render callback. Moved out of runtime/audio_mixer.c unchanged in behaviour (C8). */
#include <TargetConditionals.h>
#include "audio_out.h"
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>

struct AudioOut {
    AudioComponentInstance unit; int rate, channels;
    audio_out_pull_fn pull; void *ctx;
};

const char *audio_out_backend(void) { return "CoreAudio"; }

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    AudioOut *o = ctx;
    o->pull(o->ctx, io->mBuffers[0].mData, frames);
    return noErr;
}

static int unit_open(AudioOut *o) {
#if TARGET_OS_IPHONE
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};   /* session set up by the app */
#else
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
#endif
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &o->unit)) { fprintf(stderr, "[totk-audio] 【红】no default output device: mixing without sound\n"); return 0; }
    UInt32 bytes = (UInt32)(4 * o->channels);
    AudioStreamBasicDescription f = {(Float64)o->rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, bytes, 1, bytes, (UInt32)o->channels, 32, 0};
    AURenderCallbackStruct cb = {render, o};
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(o->unit) || AudioOutputUnitStart(o->unit)) {
        fprintf(stderr, "[totk-audio] 【红】CoreAudio output setup failed: mixing without sound\n"); AudioComponentInstanceDispose(o->unit); o->unit = NULL; return 0;
    }
    fprintf(stderr, "[totk-audio] CoreAudio output %d Hz %s\n", o->rate, o->channels == 2 ? "stereo" : "multichannel");
    return 1;
}
static void unit_close(AudioOut *o) {
    if (!o->unit) return;
    AudioOutputUnitStop(o->unit); AudioUnitUninitialize(o->unit); AudioComponentInstanceDispose(o->unit); o->unit = NULL;
}

AudioOut *audio_out_open(int rate, int channels, audio_out_pull_fn pull, void *ctx) {
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate; o->channels = channels; o->pull = pull; o->ctx = ctx;
    if (!unit_open(o)) { free(o); return NULL; }
    return o;
}
int audio_out_restart(AudioOut *o, int recreate) {
    if (!o) return 0;
    if (o->unit && !recreate && !AudioOutputUnitStart(o->unit)) return 1;
    unit_close(o);
    return unit_open(o);
}
void audio_out_close(AudioOut *o) {
    if (!o) return;
    unit_close(o); free(o);
}
