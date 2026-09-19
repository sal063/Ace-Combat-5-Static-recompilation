#include "ps2_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define SPU2_RAM_SIZE   (2u * 1024u * 1024u)
#define SPU2_VOICES     48u
#define SPU2_RATE       48000u

u8 *ps2_spu2_ram;

static const int adpcm_f0[5] = { 0, 60, 115,  98, 122 };
static const int adpcm_f1[5] = { 0,  0, -52, -55, -60 };

#define SPU2_LOOP_END   0x0100u
#define SPU2_LOOP_REP   0x0200u
#define SPU2_LOOP_START 0x0400u

static s16 clamp16(int v) {
    return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void ps2_spu2_decode_block(const u8 *blk, s16 *out, int *s1, int *s2,
                           u32 *hdr) {
    u32 h = (u32)blk[0] | ((u32)blk[1] << 8);
    u32 shift = h & 0x0Fu, filt = (h >> 4) & 0x0Fu;
    int p1 = *s1, p2 = *s2, i;
    if (filt > 4u) filt = 0u;
    if (hdr) *hdr = h;
    for (i = 0; i < 28; i++) {
        int nib = (blk[2 + (i >> 1)] >> ((i & 1) ? 4 : 0)) & 0x0F;
        int s = (int)(s16)(u16)((u32)nib << 12);
        int v;
        s = (shift > 12u) ? 0 : (s >> shift);
        v = s + ((p1 * adpcm_f0[filt] + p2 * adpcm_f1[filt]) >> 6);
        out[i] = clamp16(v);
        p2 = p1;
        p1 = out[i];
    }
    *s1 = p1;
    *s2 = p2;
}

typedef enum { ADSR_OFF, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE }
    adsr_phase;

typedef struct {
    u32 ssa, lsax, nax;
    u32 pitch;
    u32 counter;
    s16 buf[28];
    int have;
    int idx;
    s16 previous;
    int s1, s2;
    s16 voll, volr;
    u32 adsr1, adsr2;
    adsr_phase phase;
    int env;
    u32 env_clock;
    int on;
    int loop;
} spu2_voice;

static spu2_voice spu2_v[SPU2_VOICES];
static SDL_SpinLock spu2_lock;
void ps2_spu2_command_lock(void) { SDL_LockSpinlock(&spu2_lock); }
void ps2_spu2_command_unlock(void) { SDL_UnlockSpinlock(&spu2_lock); }
u8 ps2_spu2_voice_status(u32 i) {
    if(i>=48||!spu2_v[i].on) return 0;
    if(spu2_v[i].phase==ADSR_RELEASE) return spu2_v[i].env?2:0;
    return spu2_v[i].env?1:3;
}
void ps2_spu2_voice_pitch(u32 i,u32 p) { if(i<48) spu2_v[i].pitch=p&0x3fff; }
static u32 spu2_endx;
static u64 spu2_deposited;
static u64 spu2_frames;
static u64 spu2_sets, spu2_kons, spu2_koffs, spu2_sweeps;
static u64 spu2_frames_live;
static u32 spu2_peak_live;
static u64 spu2_adsr_stops;
static u64 spu2_end_stops;
static u64 spu2_frames_env;
static u64 spu2_frames_vol;

void ps2_spu2_init(void) {
    if (ps2_spu2_ram) return;
    ps2_spu2_ram = (u8 *)calloc(1, SPU2_RAM_SIZE);
    if (!ps2_spu2_ram) ps2_fatal("cannot allocate SPU2 sound RAM");
    memset(spu2_v, 0, sizeof spu2_v);
    spu2_endx = 0;
}

void ps2_spu2_write(u32 dst, u32 iop_src, u32 size) {
    u32 off;
    if (!ps2_spu2_ram) ps2_spu2_init();
    if (dst >= SPU2_RAM_SIZE) return;
    if (size > SPU2_RAM_SIZE - dst) size = SPU2_RAM_SIZE - dst;
    if (iop_src < PS2_IOP_RAM_BASE) return;
    off = iop_src - PS2_IOP_RAM_BASE;
    if (off >= PS2_IOP_RAM_SIZE) return;
    if (size > PS2_IOP_RAM_SIZE - off) size = PS2_IOP_RAM_SIZE - off;
    memcpy(ps2_spu2_ram + dst, ps2_iop_ram + off, size);
    spu2_deposited += size;
    if (getenv("PS2_SPU2_TRACE")) {
        static unsigned n;
        if (n < 64)
            ps2_log("spu2: xfer %2u  dst=%06X  size=%6u  (end %06X)",
                    n, dst, size, dst + size);
        n++;
    }
}

u64 ps2_spu2_bytes(void) { return spu2_deposited; }

static void adsr_step(spu2_voice *v) {
    u32 shift, subrate = 0, period;
    int down, exponential, delta;
    int target = (int)(((v->adsr1 & 15u) + 1u) * 0x800u);
    switch (v->phase) {
    case ADSR_ATTACK:
        shift = (v->adsr1 >> 10) & 31u; subrate = (v->adsr1 >> 8) & 3u;
        down = 0; exponential = (v->adsr1 >> 15) & 1u; break;
    case ADSR_DECAY:
        shift = (v->adsr1 >> 4) & 15u;
        down = exponential = 1; break;
    case ADSR_SUSTAIN:
        shift = (v->adsr2 >> 8) & 31u; subrate = (v->adsr2 >> 6) & 3u;
        down = (v->adsr2 >> 14) & 1u; exponential = (v->adsr2 >> 15) & 1u; break;
    case ADSR_RELEASE:
        shift = v->adsr2 & 31u;
        down = 1; exponential = (v->adsr2 >> 5) & 1u; break;
    default: return;
    }
    period = 1u << (shift > 11u ? shift - 11u : 0u);
    delta = down ? -8 + (int)subrate : 7 - (int)subrate;
    delta *= 1 << (shift < 11u ? 11u - shift : 0u);
    if (exponential && !down && v->env > 0x6000) period *= 4u;
    if (++v->env_clock < period) return;
    v->env_clock = 0;
    if (exponential && down) delta = (delta * v->env) >> 15;
    v->env += delta;
    if (v->env < 0) v->env = 0;
    if (v->env > 0x7FFF) v->env = 0x7FFF;
    if (v->phase == ADSR_ATTACK && v->env == 0x7FFF) v->phase = ADSR_DECAY;
    else if (v->phase == ADSR_DECAY && v->env <= target) v->phase = ADSR_SUSTAIN;
    else if (v->phase == ADSR_RELEASE && !v->env) {
        v->phase = ADSR_OFF; v->on = 0; spu2_adsr_stops++;
    }
}

void ps2_spu2_key_on(u32 i, u32 ssa, u32 lsax, u32 pitch,
                     u32 adsr1, u32 adsr2, s16 voll, s16 volr) {
    spu2_voice *v;
    if (i >= SPU2_VOICES) return;
    if (!ps2_spu2_ram) ps2_spu2_init();
    v = &spu2_v[i];
    v->ssa = ssa & ~0xFu;
    v->lsax = lsax & ~0xFu;
    v->nax = v->ssa;
    v->pitch = pitch & 0x3FFFu;
    v->counter = 0;
    v->have = 0; v->idx = 0; v->s1 = 0; v->s2 = 0;
    v->previous = 0;
    v->voll = voll; v->volr = volr;
    v->adsr1 = adsr1; v->adsr2 = adsr2;
    v->phase = ADSR_ATTACK; v->env = 0; v->env_clock = 0; v->on = 1;
    spu2_endx &= ~(1u << (i & 31u));
}

void ps2_spu2_key_off(u32 i) {
    if (i >= SPU2_VOICES) return;
    if (spu2_v[i].on && spu2_v[i].phase != ADSR_RELEASE) {
        spu2_v[i].phase = ADSR_RELEASE;
        spu2_v[i].env_clock = 0;
    }
}

static s16 spu2_vol_decode(u16 reg) {
    if (reg & 0x8000u) {
        spu2_sweeps++;
        return (reg & 0x2000u) ? 0 : 0x7FFF;
    }
    return (s16)((u16)(reg << 1));
}

void ps2_spu2_set_voice(u32 i, u32 ssa, u32 pitch, u32 adsr1, u32 adsr2,
                        u32 voll_reg, u32 volr_reg, int loop) {
    spu2_voice *v;
    if (i >= SPU2_VOICES) return;
    if (!ps2_spu2_ram) ps2_spu2_init();
    v = &spu2_v[i];
    v->ssa   = ssa & ~0xFu;
    v->pitch = pitch & 0x3FFFu;
    v->adsr1 = adsr1;
    v->adsr2 = adsr2;
    v->voll  = spu2_vol_decode((u16)voll_reg);
    v->volr  = spu2_vol_decode((u16)volr_reg);
    v->loop  = loop;
    spu2_sets++;
}

void ps2_spu2_stream_volume(u32 i, u32 l, u32 r) {
    if (i >= SPU2_VOICES) return;
    spu2_v[i].voll = spu2_vol_decode((u16)l);
    spu2_v[i].volr = spu2_vol_decode((u16)r);
}
void ps2_spu2_key_on_mask(u32 core0, u32 core1) {
    u32 b;
    for (b = 0; b < 24u; b++) {
        if (core0 & (1u << b)) {
            spu2_voice *v = &spu2_v[b];
            ps2_spu2_key_on(b, v->ssa, v->ssa, v->pitch, v->adsr1, v->adsr2,
                            v->voll, v->volr);
            spu2_kons++;
        }
        if (core1 & (1u << b)) {
            spu2_voice *v = &spu2_v[24u + b];
            ps2_spu2_key_on(24u + b, v->ssa, v->ssa, v->pitch, v->adsr1,
                            v->adsr2, v->voll, v->volr);
            spu2_kons++;
        }
    }
}

void ps2_spu2_key_off_mask(u32 core0, u32 core1) {
    u32 b;
    for (b = 0; b < 24u; b++) {
        if (core0 & (1u << b)) { ps2_spu2_key_off(b);        spu2_koffs++; }
        if (core1 & (1u << b)) { ps2_spu2_key_off(24u + b);  spu2_koffs++; }
    }
}

u32 ps2_spu2_endx(void) { return spu2_endx; }

static int voice_load(spu2_voice *v, u32 i) {
    if (!v->have) {
        u32 h;
        if (v->nax > SPU2_RAM_SIZE - 16u) {
            v->on = 0;
            spu2_end_stops++;
            return 0;
        }
        ps2_spu2_decode_block(ps2_spu2_ram + v->nax, v->buf, &v->s1, &v->s2,
                              &h);
        if (h & SPU2_LOOP_START) v->lsax = v->nax;
        v->have = 1;
        v->idx = 0;
        v->nax += 16u;
        if (h & SPU2_LOOP_END) {
            spu2_endx |= 1u << (i & 31u);
            v->nax = (h & SPU2_LOOP_REP) ? v->lsax : 0xFFFFFFFFu;
        }
    }
    return 1;
}

static void voice_sample(spu2_voice *v, u32 i, int *l, int *r) {
    int s;
    if (!v->on || !voice_load(v, i)) return;
    s = v->previous + (((int)v->buf[v->idx] - v->previous)
                       * (int)v->counter >> 12);
    *l += ((s * v->env) >> 15) * v->voll >> 15;
    *r += ((s * v->env) >> 15) * v->volr >> 15;
    v->counter += v->pitch;
    while (v->counter >= 0x1000u) {
        v->counter -= 0x1000u;
        if (!voice_load(v, i)) break;
        v->previous = v->buf[v->idx];
        if (++v->idx >= 28) {
            v->idx = 0;
            v->have = 0;
        }
    }
    adsr_step(v);
}

#include "ps2_nustream.h"
void ps2_spu2_mix(s16 *out, u32 n) {
    u32 k, i;
    s16 volumes[48][2];
    u64 stream_mask = ps2_nustream_voice_mask();
    if (!ps2_spu2_ram) { memset(out, 0, (size_t)n * 4u); return; }
    SDL_LockSpinlock(&spu2_lock);
    for (i = 0; i < 48; i++) {
        volumes[i][0] = spu2_v[i].voll;
        volumes[i][1] = spu2_v[i].volr;
    }
    for (k = 0; k < n; k++) {
        int l = 0, r = 0;
        u32 nlive = 0, nenv = 0, nvol = 0;
        for (i = 0; i < SPU2_VOICES; i++) {
            spu2_voice *v = &spu2_v[i];
            if (stream_mask & (1ull << i)) continue;
            if (v->on) {
                nlive++;
                if (v->env > 0) {
                    nenv++;
                    if (v->voll || v->volr) nvol++;
                }
            }
            voice_sample(v, i, &l, &r);
        }
        if (nlive) {
            spu2_frames_live++;
            if (nlive > spu2_peak_live) spu2_peak_live = nlive;
        }
        if (nenv) spu2_frames_env++;
        if (nvol) spu2_frames_vol++;
        out[k * 2 + 0] = clamp16(l);
        out[k * 2 + 1] = clamp16(r);
    }
    spu2_frames += n;
    SDL_UnlockSpinlock(&spu2_lock);
    ps2_nustream_mix(out, n, volumes);
}

void ps2_spu2_report(void) {
    u32 i, live = 0;
    for (i = 0; i < SPU2_VOICES; i++) if (spu2_v[i].on) live++;
    ps2_log("spu2: %llu bytes into sound RAM, %llu frames mixed, %u voices live",
            (unsigned long long)spu2_deposited,
            (unsigned long long)spu2_frames, live);
    ps2_log("spu2: %llu tone-attribute sets, %llu key-ons, %llu key-offs",
            (unsigned long long)spu2_sets, (unsigned long long)spu2_kons,
            (unsigned long long)spu2_koffs);
    if (spu2_sweeps)
        ps2_log("spu2: %llu volume registers were in sweep mode and were taken "
                "at a steady magnitude -- the ramp is milestone M4",
                (unsigned long long)spu2_sweeps);
    if (spu2_frames)
        ps2_log("spu2: a voice was sounding for %llu of %llu mixed frames "
                "(%.2f%%), at most %u at once",
                (unsigned long long)spu2_frames_live,
                (unsigned long long)spu2_frames,
                100.0 * (double)spu2_frames_live / (double)spu2_frames,
                spu2_peak_live);
    if (spu2_frames_live)
        ps2_log("spu2:   of those, %llu had a non-zero envelope and %llu also "
                "a non-zero volume",
                (unsigned long long)spu2_frames_env,
                (unsigned long long)spu2_frames_vol);
    if (spu2_adsr_stops || spu2_end_stops)
        ps2_log("spu2: voices stopped -- %llu by the envelope releasing to "
                "zero, %llu by running off a non-repeating waveform",
                (unsigned long long)spu2_adsr_stops,
                (unsigned long long)spu2_end_stops);
    if (spu2_kons && spu2_frames &&
        (double)spu2_frames_live / (double)spu2_frames < 0.10) {
        ps2_log("spu2: %llu key-ons produced only %.2f%% sounding frames",
                (unsigned long long)spu2_kons,
                100.0 * (double)spu2_frames_live / (double)spu2_frames);
        if (spu2_end_stops > spu2_adsr_stops)
            ps2_log("spu2:   mostly end-of-waveform -- look at looping first. "
                    "The tone-attribute record carries a loop flag at +28 "
                    "which this file stores but does not yet act on (open "
                    "question 4 in docs/AUDIO_PLAN.md)");
        else
            ps2_log("spu2:   mostly envelope releases");
    }
}

static int spu2_fail;

static void spu2_expect(const char *what, int got, int want) {
    if (got != want) {
        spu2_fail++;
        ps2_log("  FAIL %-46s got %d want %d", what, got, want);
    }
}

int ps2_spu2_selftest(void) {
    u8 blk[16];
    s16 out[28];
    int s1, s2, i;
    u32 h;

    ps2_spu2_init();
    spu2_fail = 0;
    ps2_log("---- SPU2 self-test (SPU2_Overview_Manual.pdf p.15-16, p.20) ----");
    {
        spu2_voice v = {0};
        int l = 0, r = 0;
        memset(ps2_spu2_ram, 0, 64);
        memset(ps2_spu2_ram + 2, 0x11, 14);
        memset(ps2_spu2_ram + 18, 0x22, 14);
        v.on = 1; v.pitch = 0x800; v.env = 32767;
        v.voll = spu2_vol_decode(0x2000); v.volr = spu2_vol_decode(0x6000);
        voice_sample(&v, 0, &l, &r);
        spu2_expect("interpolation starts at zero history", l, 0);
        voice_sample(&v, 0, &l, &r);
        spu2_expect("half-sample interpolates instead of repeating", l, 1023);
        spu2_expect("rear phase survives in opposite channel", r, -1024);
        v = (spu2_voice){0}; v.on = 1; v.pitch = 0x3800;
        for (int k = 0; k < 10; k++) voice_sample(&v, 0, &l, &r);
        spu2_expect("high pitch consumes all steps across block", v.idx, 7);
        spu2_expect("high pitch retains only fractional phase", v.counter, 0);
        spu2_expect("high pitch loads next ADPCM block", v.nax, 32);
        spu2_expect("interpolation history crosses block", v.previous, 8192);
        memset(ps2_spu2_ram, 0, 64);
    }
    {
        spu2_voice v = {0};
        v.phase = ADSR_ATTACK; v.adsr1 = 0x000f;
        adsr_step(&v); spu2_expect("fast attack first step", v.env, 14336);
        adsr_step(&v); adsr_step(&v);
        spu2_expect("attack reaches peak", v.env, 32767);
        adsr_step(&v);
        spu2_expect("decay reaches sustain", v.phase, ADSR_SUSTAIN);
        v.env = 16000; v.adsr2 = 0x0000;
        adsr_step(&v);
        spu2_expect("linear increasing sustain", v.env, 30336);
        v.env = 16000; v.adsr2 = 0x4c00; v.env_clock = 0;
        adsr_step(&v); spu2_expect("slow sustain waits for rate", v.env, 16000);
        adsr_step(&v); spu2_expect("decreasing sustain step", v.env, 15992);
        v.env = 16000; v.adsr2 = 0xc000; v.env_clock = 0;
        adsr_step(&v); spu2_expect("exponential sustain", v.env, 8000);
    }

    memset(blk, 0, sizeof blk);
    blk[0] = 0x00;
    blk[1] = 0x00;
    for (i = 0; i < 14; i++) blk[2 + i] = 0x21;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("filter 0 shift 0, nibble 1", out[0], 0x1000);
    spu2_expect("filter 0 shift 0, nibble 2", out[1], 0x2000);
    spu2_expect("header decoded", (int)h, 0);

    blk[0] = 0x04;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("shift 4 scales the sample down", out[0], 0x0100);

    memset(blk + 2, 0xFF, 14);
    blk[0] = 0x00;
    s1 = s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("nibble F is negative", out[0], -0x1000);

    memset(blk + 2, 0, 14);
    blk[0] = 0x10;
    s1 = 0x1000; s2 = 0;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("filter 1 carries 60/64 of the previous sample",
                out[0], (0x1000 * 60) >> 6);

    blk[0] = 0x00; blk[1] = 0x07;
    ps2_spu2_decode_block(blk, out, &s1, &s2, &h);
    spu2_expect("LOOP/END   is bit 8",  (h & SPU2_LOOP_END)   ? 1 : 0, 1);
    spu2_expect("LOOP       is bit 9",  (h & SPU2_LOOP_REP)   ? 1 : 0, 1);
    spu2_expect("LOOP/START is bit 10", (h & SPU2_LOOP_START) ? 1 : 0, 1);

    {
        s16 mix[256 * 2];
        int nonzero = 0;
        memset(ps2_spu2_ram, 0, 4096);
        ps2_spu2_ram[0] = 0x00;
        ps2_spu2_ram[1] = 0x03;
        for (i = 0; i < 14; i++) ps2_spu2_ram[2 + i] = 0x71;
        ps2_spu2_key_on(0, 0, 0, 0x1000, 0x000F, 0x0000, 0x3FFF, 0x3FFF);
        ps2_spu2_mix(mix, 256);
        for (i = 0; i < 256 * 2; i++) if (mix[i]) nonzero++;
        spu2_expect("a keyed-on voice produces sound", nonzero > 0, 1);
        spu2_expect("ENDX latches at the endpoint block",
                    (ps2_spu2_endx() & 1u) ? 1 : 0, 1);
        ps2_spu2_key_off(0);
        memset(ps2_spu2_ram, 0, 4096);
    }

    {
        s16 mix[64 * 2];
        memset(ps2_spu2_ram, 0, 4096);
        ps2_spu2_ram[1] = 0x03;
        for (i = 0; i < 14; i++) ps2_spu2_ram[2 + i] = 0x71;
        ps2_spu2_set_voice(0, 0, 0, 0x000F, 0x0000, 0x3FFF, 0x3FFF, 1);
        spu2_expect("tone attribute keeps pitch 0", (int)spu2_v[0].pitch, 0);
        ps2_spu2_key_on_mask(1u, 0u);
        spu2_expect("key-on keeps pitch 0", (int)spu2_v[0].pitch, 0);
        ps2_spu2_mix(mix, 64);
        spu2_expect("pitch 0 holds the sample position", spu2_v[0].idx, 0);
        ps2_spu2_set_voice(0, 0, 0x1000, 0x000F, 0x0000, 0x3FFF, 0x3FFF, 1);
        ps2_spu2_mix(mix, 10);
        spu2_expect("restored pitch resumes from the held position",
                    spu2_v[0].idx, 10);
        ps2_spu2_key_off(0);
        memset(ps2_spu2_ram, 0, 4096);
    }

    {
        s16 mix[8];
        const u32 bad[] = {0xffffffffu, 0xfffffff0u, SPU2_RAM_SIZE};
        for (u32 j = 0; j < sizeof bad / sizeof bad[0]; j++) {
            ps2_spu2_key_on(47, bad[j], 0, 0x1000, 15, 0, 0x3fff, 0x3fff);
            ps2_spu2_mix(mix, 4);
            spu2_expect("invalid waveform address mutes safely", spu2_v[47].on, 0);
        }
    }
    extern int ps2_nustream_selftest(void);
    spu2_fail += ps2_nustream_selftest();
    ps2_log("spu2 selftest: %s", spu2_fail ? "FAILED" : "all checks passed");
    return spu2_fail ? 1 : 0;
}
