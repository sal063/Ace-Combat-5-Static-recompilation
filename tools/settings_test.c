#include "ps2_runtime.h"
#include "ps2_settings.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8 *ps2_pt[PS2_PT_ENTRIES];
u8 *ps2_ram;
u8 *ps2_spr;
void ps2_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
u8   ps2_mmio_r8(u32 a) { (void)a; abort(); }
u16  ps2_mmio_r16(u32 a) { (void)a; abort(); }
u32  ps2_mmio_r32(u32 a) { (void)a; abort(); }
u64  ps2_mmio_r64(u32 a) { (void)a; abort(); }
void ps2_mmio_r128(ps2_reg128 *d, u32 a) { (void)d; (void)a; abort(); }
void ps2_mmio_w8(u32 a, u8 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w16(u32 a, u16 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w32(u32 a, u32 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w64(u32 a, u64 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w128(u32 a, const ps2_reg128 *v) { (void)a; (void)v; abort(); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static u8 old_axis_to_pad(int v, int dz) {
    int span = 32767 - dz, m;
    if (v > -dz && v < dz) return 128;
    if (span < 1) span = 1;
    m = v > 0 ? v - dz : v + dz;
    m = m * 32767 / span;
    if (m < -32768) m = -32768;
    if (m > 32767) m = 32767;
    m = (m + 32768) >> 8;
    if (m < 0) m = 0;
    if (m > 255) m = 255;
    return (u8)m;
}

static u8 new_axis_to_pad(int v, const ps2_stick_cfg *c) {
    int pos = v < 0 ? 0 : v > 32767 ? 32767 : v;
    int neg = -v < 0 ? 0 : -v > 32767 ? 32767 : -v;
    float x = (float)pos / 32767.0f - (float)neg / 32767.0f, ox, oy;
    int m;
    ps2_stick_process(c, x, 0.0f, &ox, &oy);
    m = ox <= -1.0f ? -32768 : (int)(ox * 32767.0f);
    m = (m + 32768) >> 8;
    return (u8)(m < 0 ? 0 : m > 255 ? 255 : m);
}

static int feq(float a, float b) { return fabsf(a - b) < 1e-5f; }

int main(void) {
    static u8 ram[PS2_RAM_SIZE];
    for (unsigned p = 0; p < PS2_RAM_SIZE / PS2_PAGE_SIZE; p++) ps2_pt[p] = ram + p * PS2_PAGE_SIZE;
    ps2_ram = ram;

    ps2_settings_defaults(&ps2_cfg);
    CHECK(ps2_cfg.preset == PS2_PRESET_LOW, "defaults are not Low");
    CHECK(ps2_settings_preset_matches(&ps2_cfg) == PS2_PRESET_LOW, "defaults do not match Low");
    CHECK(ps2_cfg.anisotropy == 0 && ps2_cfg.deinterlace == 1
          && ps2_cfg.fxaa == 0 && ps2_cfg.sharpen == 0.0f && ps2_cfg.scale_filter == PS2_SCALE_BILINEAR
          && ps2_cfg.aspect == PS2_ASPECT_STRETCH && ps2_cfg.present_mode == PS2_PRESENT_MAILBOX
          && ps2_cfg.window_w == 1280 && ps2_cfg.window_h == 896 && ps2_cfg.widescreen == 0
          && ps2_cfg.internal_res == 1,
          "defaults differ from the pre-menu renderer");
    for (int i = 0; i <= PS2_PRESET_ULTRA; i++) {
        ps2_settings s;
        ps2_settings_defaults(&s);
        ps2_settings_apply_preset(&s, i);
        CHECK(ps2_settings_preset_matches(&s) == i, "preset %d does not round-trip", i);
    }
    {
        ps2_settings s;
        ps2_settings_defaults(&s);
        s.anisotropy = 4;
        CHECK(ps2_settings_preset_matches(&s) == PS2_PRESET_CUSTOM, "edited preset not Custom");
        static const int res[4] = { 1, 2, 3, 4 };
        for (int i = 0; i <= PS2_PRESET_ULTRA; i++) {
            ps2_settings_apply_preset(&s, i);
            CHECK(s.internal_res == res[i], "preset %d internal resolution %d", i, s.internal_res);
        }
        ps2_settings_apply_preset(&s, PS2_PRESET_HIGH);
        s.internal_res = 0;
        CHECK(ps2_settings_preset_matches(&s) == PS2_PRESET_CUSTOM, "Auto resolution kept High");
    }

    CHECK(ps2_cfg.key[PS2_ACT_CROSS][0] == SDL_SCANCODE_X && ps2_cfg.key[PS2_ACT_START][0] == SDL_SCANCODE_RETURN
          && ps2_cfg.key[PS2_ACT_LS_UP][0] == SDL_SCANCODE_W && ps2_cfg.key[PS2_ACT_LS_UP][1] == SDL_SCANCODE_KP_8,
          "default keyboard map");
    CHECK(ps2_cfg.pad[PS2_ACT_CROSS][0] == PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_SOUTH)
          && ps2_cfg.pad[PS2_ACT_R2][0] == PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)
          && ps2_cfg.pad[PS2_ACT_LS_LEFT][0] == PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTX, 0),
          "default controller map");

    {
        ps2_settings a;
        ps2_settings_defaults(&a);
        a.anisotropy = 8; a.fxaa = 1; a.sharpen = 0.3f; a.scale_filter = PS2_SCALE_SHARP;
        a.brightness = -0.12f; a.gamma = 1.3f; a.window_mode = PS2_WIN_EXCLUSIVE;
        a.fullscreen_type = PS2_WIN_EXCLUSIVE; a.fs_w = 1920; a.fs_h = 1080; a.fs_hz = 143.98f;
        a.aspect = PS2_ASPECT_CUSTOM; a.aspect_custom = 2.1f; a.widescreen = 1; a.integer_scale = 1;
        a.present_mode = PS2_PRESENT_IMMEDIATE; a.fps_limit = 90; a.show_fps = 1; a.ui_scale = 1.25f;
        a.key[PS2_ACT_CROSS][1] = SDL_SCANCODE_SPACE;
        a.pad[PS2_ACT_R2][1] = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        a.stick[1].shape = PS2_DZ_SCALED_RADIAL; a.stick[1].inner = 0.1f; a.stick[1].outer = 0.05f;
        a.stick[0].curve = 1.5f; a.stick[0].invert_y = 1; a.trigger_press = 0.5f;
        a.internal_res = 0;
        a.preset = ps2_settings_preset_matches(&a);
        ps2_cfg = a;
        ps2_settings_save();
        memset(&ps2_cfg, 0, sizeof ps2_cfg);
        ps2_settings_load();
        ps2_settings *b = &ps2_cfg;
        CHECK(b->preset == a.preset && b->anisotropy == 8 && b->fxaa == 1 && feq(b->sharpen, 0.3f)
              && b->scale_filter == PS2_SCALE_SHARP && feq(b->brightness, -0.12f) && feq(b->gamma, 1.3f),
              "graphics fields did not round-trip");
        CHECK(b->window_mode == PS2_WIN_EXCLUSIVE && b->fullscreen_type == PS2_WIN_EXCLUSIVE && b->fs_w == 1920
              && b->fs_h == 1080 && feq(b->fs_hz, 143.98f) && b->aspect == PS2_ASPECT_CUSTOM
              && feq(b->aspect_custom, 2.1f) && b->widescreen == 1 && b->integer_scale == 1
              && b->present_mode == PS2_PRESENT_IMMEDIATE && b->fps_limit == 90 && b->show_fps == 1
              && feq(b->ui_scale, 1.25f), "display fields did not round-trip");
        CHECK(b->internal_res == 0, "internal resolution did not round-trip (%d)", b->internal_res);
        CHECK(!memcmp(b->key, a.key, sizeof a.key) && !memcmp(b->pad, a.pad, sizeof a.pad),
              "bindings did not round-trip");
        CHECK(b->stick[1].shape == PS2_DZ_SCALED_RADIAL && feq(b->stick[1].inner, 0.1f)
              && feq(b->stick[1].outer, 0.05f) && feq(b->stick[0].curve, 1.5f) && b->stick[0].invert_y == 1
              && feq(b->trigger_press, 0.5f), "analog fields did not round-trip");
        ps2_settings d;
        ps2_settings_defaults(&d);
        CHECK((int)(b->trigger_deadzone * 32767.0f + 0.5f) == 3200
              && (int)(d.trigger_press * 32767.0f + 0.5f) == 8000, "trigger thresholds drifted");
    }

    {
        FILE *f = fopen(ps2_settings_path(), "w");
        fprintf(f, "[graphics]\nanisotropy = 7\npreset = 3\n[display]\nwindow_width = 10\naspect = 99\n"
                   "[analog]\nleft_deadzone = 5\nleft_shape = -4\n[keyboard]\ncross = 999999 -3\n");
        fclose(f);
        ps2_settings_load();
        CHECK(ps2_cfg.anisotropy == 0 && ps2_cfg.window_w == 320 && ps2_cfg.aspect == 4
              && feq(ps2_cfg.stick[0].inner, 0.9f) && ps2_cfg.stick[0].shape == 0
              && ps2_cfg.key[PS2_ACT_CROSS][0] == 0 && ps2_cfg.key[PS2_ACT_CROSS][1] == 0,
              "hand-edited values were not clamped");
        CHECK(ps2_cfg.preset == PS2_PRESET_LOW, "a preset name that the fields contradict was kept (%d)",
              ps2_cfg.preset);
        f = fopen(ps2_settings_path(), "w");
        fprintf(f, "[graphics]\ninternal_resolution = 99\n");
        fclose(f);
        ps2_settings_load();
        CHECK(ps2_cfg.internal_res == PS2_INTERNAL_RES_MAX && ps2_cfg.preset == PS2_PRESET_CUSTOM,
              "internal resolution 99 was not clamped (%d)", ps2_cfg.internal_res);
        f = fopen(ps2_settings_path(), "w");
        fprintf(f, "[graphics]\ninternal_resolution = -3\n");
        fclose(f);
        ps2_settings_load();
        CHECK(ps2_cfg.internal_res == 0, "internal resolution -3 was not clamped (%d)", ps2_cfg.internal_res);
    }

    {
        ps2_settings d;
        int worst[2] = {0, 0}, diffs[2] = {0, 0};
        ps2_settings_defaults(&d);
        for (int s = 0; s < 2; s++) {
            int dz = s ? 8689 : 7849;
            for (int v = -32768; v <= 32767; v++) {
                int o = old_axis_to_pad(v, dz), n = new_axis_to_pad(v, &d.stick[s]);
                int e = abs(o - n);
                if (e) diffs[s]++;
                if (e > worst[s]) worst[s] = e;
            }
            printf("stick %d: %d of 65536 axis values differ from the old mapping, by at most %d\n",
                   s, diffs[s], worst[s]);
            CHECK(worst[s] <= 1, "stick %d differs from the old mapping by %d", s, worst[s]);
            CHECK(new_axis_to_pad(0, &d.stick[s]) == 128 && new_axis_to_pad(32767, &d.stick[s]) == 255
                  && new_axis_to_pad(-32768, &d.stick[s]) == 0, "stick %d centre/extremes", s);
        }
    }

    {
        ps2_stick_cfg c = { PS2_DZ_SCALED_RADIAL, 0.2f, 0.0f, 1.0f, 0, 0 };
        float ox, oy;
        ps2_stick_process(&c, 0.1f, 0.1f, &ox, &oy);
        CHECK(ox == 0.0f && oy == 0.0f, "scaled radial inside the dead zone");
        ps2_stick_process(&c, 0.7071f, 0.7071f, &ox, &oy);
        CHECK(fabsf(ox - oy) < 1e-4f && ox > 0.69f, "scaled radial full diagonal (%f,%f)", ox, oy);
        ps2_stick_process(&c, 0.3f, 0.0f, &ox, &oy);
        CHECK(fabsf(ox - 0.125f) < 1e-3f, "scaled radial rescale (%f)", ox);
        c.shape = PS2_DZ_RADIAL;
        ps2_stick_process(&c, 0.3f, 0.0f, &ox, &oy);
        CHECK(fabsf(ox - 0.3f) < 1e-3f, "radial keeps magnitude (%f)", ox);
        c.shape = PS2_DZ_AXIAL; c.invert_x = 1;
        ps2_stick_process(&c, 1.0f, 0.0f, &ox, &oy);
        CHECK(ox == -1.0f, "invert x");
    }

    {
        ps2_w32(0x00440828u, 0x440315C2u);
        ps2_w32(0x0044082Cu, 0x441013D7u);
        ps2_cfg.widescreen = 1;
        ps2_settings_apply_game_patches();
        CHECK(ps2_r32(0x00440828u) == 0x43D638F3u && ps2_r32(0x0044082Cu) == 0x43EB7385u, "patch not applied");
        ps2_cfg.widescreen = 0;
        ps2_settings_apply_game_patches();
        CHECK(ps2_r32(0x00440828u) == 0x440315C2u && ps2_r32(0x0044082Cu) == 0x441013D7u, "patch not removed");
        ps2_w32(0x00440828u, 0x12345678u);
        ps2_cfg.widescreen = 1;
        ps2_settings_apply_game_patches();
        CHECK(ps2_r32(0x00440828u) == 0x12345678u, "patch overwrote an unexpected value");
    }

    printf("settings test: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
