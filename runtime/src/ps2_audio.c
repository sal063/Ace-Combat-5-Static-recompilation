#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_runtime.h"

void ps2_spu2_mix(s16 *out, u32 n);
void ps2_spu2_init(void);

#define AUDIO_RATE      48000
#define AUDIO_CHANNELS  2
#define AUDIO_CHUNK     512

static SDL_AudioStream *audio_stream;
static u64 audio_bytes;
static int audio_started;
static FILE *wav_fp;
static u32 wav_bytes;

static void wav_open(void) {
    const char *path = getenv("PS2_AUDIO_WAV");
    u8 hdr[44];
    if (!path) return;
    wav_fp = fopen(path, "wb");
    if (!wav_fp) { ps2_log("audio: cannot open %s", path); return; }
    memset(hdr, 0, sizeof hdr);
    fwrite(hdr, 1, sizeof hdr, wav_fp);
    ps2_log("audio: tapping the mix to %s", path);
}

static void wav_put(const void *buf, int len) {
    if (!wav_fp) return;
    fwrite(buf, 1, (size_t)len, wav_fp);
    wav_bytes += (u32)len;
}

static void wav_close(void) {
    u8 h[44];
    u32 v;
    if (!wav_fp) return;
    memcpy(h, "RIFF", 4);   v = 36u + wav_bytes;      memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;                 memcpy(h + 16, &v, 4);
    { u16 t = 1;            memcpy(h + 20, &t, 2);
      t = AUDIO_CHANNELS;   memcpy(h + 22, &t, 2); }
    v = AUDIO_RATE;         memcpy(h + 24, &v, 4);
    v = AUDIO_RATE * AUDIO_CHANNELS * 2u; memcpy(h + 28, &v, 4);
    { u16 t = AUDIO_CHANNELS * 2u; memcpy(h + 32, &t, 2);
      t = 16;               memcpy(h + 34, &t, 2); }
    memcpy(h + 36, "data", 4);            memcpy(h + 40, &wav_bytes, 4);
    fseek(wav_fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav_fp);
    fclose(wav_fp);
    wav_fp = NULL;
}

static void SDLCALL audio_cb(void *userdata, SDL_AudioStream *stream,
                             int additional_amount, int total_amount) {
    s16 buf[AUDIO_CHUNK * AUDIO_CHANNELS];
    (void)userdata;
    (void)total_amount;
    while (additional_amount > 0) {
        int want_bytes = additional_amount;
        int frames, bytes;
        if (want_bytes > (int)sizeof buf) want_bytes = (int)sizeof buf;
        frames = want_bytes / (int)(AUDIO_CHANNELS * sizeof(s16));
        if (frames <= 0) break;
        ps2_spu2_mix(buf, (u32)frames);
        bytes = frames * (int)(AUDIO_CHANNELS * sizeof(s16));
        SDL_PutAudioStreamData(stream, buf, bytes);
        wav_put(buf, bytes);
        audio_bytes += (u64)bytes;
        additional_amount -= bytes;
    }
}

void ps2_audio_start(void) {
    SDL_AudioSpec spec;
    if (audio_started) return;
    if (getenv("PS2_NO_AUDIO")) {
        ps2_log("audio: disabled by PS2_NO_AUDIO");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        ps2_log("audio: SDL_InitSubSystem(AUDIO): %s", SDL_GetError());
        return;
    }
    ps2_spu2_init();
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = AUDIO_CHANNELS;
    spec.freq = AUDIO_RATE;
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &spec, audio_cb, NULL);
    if (!audio_stream) {
        ps2_log("audio: SDL_OpenAudioDeviceStream: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio_stream);
    wav_open();
    audio_started = 1;
    ps2_log("audio: %u Hz %u-channel s16 device open, SPU2 mixer attached",
            AUDIO_RATE, AUDIO_CHANNELS);
}

int ps2_audio_running(void) { return audio_started; }
void ps2_audio_stop(void) {
    if (!audio_started) return;
    SDL_DestroyAudioStream(audio_stream);
    audio_stream = NULL;
    audio_started = 0;
    wav_close();
}

void ps2_audio_report(void) {
    if (!audio_started && !audio_bytes) return;
    ps2_log("audio: %llu bytes drained by the device (%.1f s at %u Hz)",
            (unsigned long long)audio_bytes,
            (double)audio_bytes / (double)(AUDIO_RATE * AUDIO_CHANNELS
                                           * (int)sizeof(s16)),
            AUDIO_RATE);
}
