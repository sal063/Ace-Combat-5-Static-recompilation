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

static int feq(float a, float b) { return fabsf(a - b) < 1e-5f; }

static float game_axis(u8 b) {
    float f = b < 127 ? (127 - b) / 127.0f : (b - 127) / 128.0f;
    return f <= 0.5f ? 0.0f : (f - 0.5f) / 0.5f;
}

static void our_stick(float dz, float scale, int diagonal, int invert, int x, int y, u8 *ox, u8 *oy) {
    const float v[4] = { ps2_pad_axis_value(y, 0), ps2_pad_axis_value(y, 1),
                         ps2_pad_axis_value(x, 0), ps2_pad_axis_value(x, 1) };
    ps2_pad_stick(dz, scale, diagonal, invert, v, ox, oy);
}

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
    CHECK(ps2_cfg.deadzone == 0.0f && ps2_cfg.axis_scale == 1.0f && ps2_cfg.button_deadzone == 0.0f
          && ps2_cfg.invert[0] == 0 && ps2_cfg.invert[1] == 0 && ps2_cfg.diagonal == 1,
          "analog defaults");

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
        a.deadzone = 0.15f; a.axis_scale = 1.4f; a.button_deadzone = 0.2f;
        a.invert[0] = 2; a.invert[1] = 3; a.diagonal = 0;
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
        CHECK(feq(b->deadzone, 0.15f) && feq(b->axis_scale, 1.4f) && feq(b->button_deadzone, 0.2f)
              && b->invert[0] == 2 && b->invert[1] == 3 && b->diagonal == 0,
              "analog fields did not round-trip");
    }

    {
        FILE *f = fopen(ps2_settings_path(), "w");
        fprintf(f, "[graphics]\nanisotropy = 7\npreset = 3\n[display]\nwindow_width = 10\naspect = 99\n"
                   "[analog]\ndeadzone = 5\naxis_scale = -4\nbutton_deadzone = -1\ninvert_left = 9\n"
                   "[keyboard]\ncross = 999999 -3\n");
        fclose(f);
        ps2_settings_load();
        CHECK(ps2_cfg.anisotropy == 0 && ps2_cfg.window_w == 320 && ps2_cfg.aspect == 4
              && ps2_cfg.key[PS2_ACT_CROSS][0] == 0 && ps2_cfg.key[PS2_ACT_CROSS][1] == 0,
              "hand-edited values were not clamped");
        CHECK(feq(ps2_cfg.deadzone, 1.0f) && feq(ps2_cfg.axis_scale, 0.01f)
              && ps2_cfg.button_deadzone == 0.0f && ps2_cfg.invert[0] == 3,
              "hand-edited analog values were not clamped");
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
        FILE *f = fopen(ps2_settings_path(), "w");
        fprintf(f, "[analog]\nleft_shape = 0\nleft_deadzone = 0.268\nright_deadzone = 0.527\n"
                   "left_invert_x = 1\nleft_invert_y = 0\nright_invert_y = 1\n"
                   "trigger_press = 0.24\nstick_as_button = 0.5\n");
        fclose(f);
        ps2_settings_load();
        CHECK(ps2_cfg.deadzone == 0.0f && ps2_cfg.axis_scale == 1.0f && ps2_cfg.button_deadzone == 0.0f,
              "an old dead zone key changed the analog settings");
        CHECK(ps2_cfg.invert[0] == 1 && ps2_cfg.invert[1] == 2,
              "old per-stick inverts did not carry over (%d, %d)", ps2_cfg.invert[0], ps2_cfg.invert[1]);
    }

    {
        static const float dzs[] = { 0.0f, 0.1f, 0.25f };
        static const float scales[] = { 0.5f, 1.0f, 1.33f, 2.0f };
        long checked = 0;
        for (int dg = 0; dg < 2; dg++)
        for (size_t a = 0; a < sizeof dzs / sizeof dzs[0]; a++)
        for (size_t b = 0; b < sizeof scales / sizeof scales[0]; b++)
        for (int deg = 0; deg < 360; deg += 5) {
            float t = (float)deg * 3.14159265f / 180.0f, cx = cosf(t), cy = sinf(t);
            int lx = 0, ly = 0;
            u8 x = 127, y = 127;
            for (int k = 0; k <= 100; k++) {
                float r = (float)k / 100.0f;
                const float v[4] = { fmaxf(-cy * r, 0.0f), fmaxf(cy * r, 0.0f),
                                     fmaxf(-cx * r, 0.0f), fmaxf(cx * r, 0.0f) };
                int ox, oy;
                ps2_pad_stick(dzs[a], scales[b], dg, 0, v, &x, &y);
                ox = x - 127;
                oy = y - 127;
                checked++;
                if (k == 0) CHECK(ox == 0 && oy == 0, "centre is not 0x7F (%u, %u)", x, y);
                CHECK(abs(ox) >= abs(lx) && abs(oy) >= abs(ly),
                      "stick at %d degrees moved back as it was pushed further (%d%%)", deg, k);
                CHECK(ox * cx >= -1e-3f && oy * cy >= -1e-3f,
                      "stick at %d degrees came out on the wrong side (%u, %u)", deg, x, y);
                lx = ox;
                ly = oy;
            }
            if (dg && scales[b] >= 1.0f)
                CHECK(lx == -127 || lx == 128 || ly == -127 || ly == 128,
                      "full deflection at %d degrees did not reach the edge (%d, %d)", deg, lx, ly);
            if (dg || scales[b] <= 1.0f) {
                float gx = copysignf(game_axis(x), (float)lx), gy = copysignf(game_axis(y), (float)ly);
                float da = fabsf(atan2f(gy, gx) - atan2f(cy, cx));
                if (da > 3.14159265f) da = 2.0f * 3.14159265f - da;
                CHECK(da < 1.5f * 3.14159265f / 180.0f,
                      "full deflection at %d degrees came out at another angle (%u, %u)", deg, x, y);
            }
        }
        printf("stick: %ld positions checked\n", checked);
    }

    {
        ps2_settings d;
        ps2_settings_defaults(&d);
        for (int deg = 0; deg < 360; deg += 5) {
            float t = (float)deg * 3.14159265f / 180.0f, cx = cosf(t), cy = sinf(t);
            float m = fmaxf(fabsf(cx), fabsf(cy));
            for (int k = 0; k <= 100; k += 5) {
                float r = (float)k / 100.0f;
                const float v[4] = { fmaxf(-cy * r, 0.0f), fmaxf(cy * r, 0.0f),
                                     fmaxf(-cx * r, 0.0f), fmaxf(cx * r, 0.0f) };
                u8 x, y;
                ps2_pad_stick(d.deadzone, d.axis_scale, d.diagonal, 0, v, &x, &y);
                CHECK(fabsf(game_axis(x) - fabsf(cx) * r / m) < 0.03f
                      && fabsf(game_axis(y) - fabsf(cy) * r / m) < 0.03f,
                      "past the game's dead zone, %d%% at %d degrees reads %.2f, %.2f",
                      k, deg, (double)game_axis(x), (double)game_axis(y));
            }
        }
    }

    {
        u8 x, y, p;
        CHECK(ps2_pad_axis_value(-32768, 0) == 1.0f && ps2_pad_axis_value(32767, 1) == 1.0f
              && ps2_pad_axis_value(-1, 1) == 0.0f && ps2_pad_axis_value(16384, 0) == 0.0f,
              "axis values");
        our_stick(0.0f, 1.33f, 0, 0, 32767, 32767, &x, &y);
        CHECK(x == 255 && y == 255, "right and down (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 0, -32768, -32768, &x, &y);
        CHECK(x == 0 && y == 0, "left and up (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 0, 16384, 0, &x, &y);
        CHECK(fabsf(game_axis(x) - 0.665f) < 0.02f && y == 127, "half right at 133%% (%u)", x);
        our_stick(0.0f, 1.33f, 0, 0, 24700, 0, &x, &y);
        CHECK(x == 255, "133%% does not saturate by 75.4%% of travel (%u)", x);
        our_stick(0.25f, 1.0f, 0, 0, 6553, 0, &x, &y);
        CHECK(x == 127 && y == 127, "20%% inside a 25%% dead zone (%u)", x);
        our_stick(0.25f, 1.0f, 0, 0, 9830, 0, &x, &y);
        CHECK(fabsf(game_axis(x) - 0.0667f) < 0.02f,
              "30%% outside a 25%% dead zone does not start again from zero (%u)", x);
        our_stick(0.25f, 1.0f, 0, 0, 6553, 6553, &x, &y);
        CHECK(x > 127 && x == y, "the dead zone is not radial (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 1, 32767, 32767, &x, &y);
        CHECK(x == 0 && y == 255, "invert left/right (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 2, 32767, 32767, &x, &y);
        CHECK(x == 255 && y == 0, "invert up/down (%u, %u)", x, y);
        {
            const float centre[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float both[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            const float tiny[4] = { 0.0f, 0.0f, 0.0f, 0.02f };
            const float diag[4] = { 0.0f, 0.02f, 0.0f, 0.1f };
            const float corner[4] = { 0.0f, 0.7071f, 0.0f, 0.7071f };
            ps2_pad_stick(0.0f, 1.33f, 0, 0, both, &x, &y);
            CHECK(x == 127 && y == 127, "opposing directions do not cancel (%u, %u)", x, y);
            ps2_pad_stick(0.0f, 1.0f, 1, 0, centre, &x, &y);
            CHECK(x == 127 && y == 127, "a centred stick moved (%u, %u)", x, y);
            ps2_pad_stick(0.0f, 1.0f, 0, 0, tiny, &x, &y);
            CHECK(x >= 191 && y == 127, "2%% right did not start past the game's dead zone (%u)", x);
            ps2_pad_stick(0.0f, 1.0f, 0, 0, diag, &x, &y);
            CHECK(y >= 191 && y < x, "the axes did not skip the game's dead zone apart (%u, %u)", x, y);
            ps2_pad_stick(0.25f, 1.0f, 0, 0, tiny, &x, &y);
            CHECK(x == 127 && y == 127, "a stick inside the dead zone moved (%u)", x);
            ps2_pad_stick(0.0f, 1.0f, 1, 0, corner, &x, &y);
            CHECK(x == 255 && y == 255, "diagonal correction did not reach the corner (%u, %u)", x, y);
            CHECK(feq(ps2_pad_past_deadzone(0.1f), 0.55f) && ps2_pad_past_deadzone(0.0f) == 0.0f
                  && feq(ps2_pad_past_deadzone(-0.6f), -0.8f) && feq(ps2_pad_past_deadzone(1.0f), 1.0f),
                  "past the game's dead zone");
        }

        CHECK(ps2_pad_trigger(0.0f, 0.5f, &p) == 1 && p == 127, "half trigger (%u)", p);
        CHECK(ps2_pad_trigger(0.0f, 0.0f, &p) == 0 && p == 0, "released trigger");
        CHECK(ps2_pad_trigger(0.0f, 1.0f / 32767.0f, &p) == 1 && p == 0, "a trigger is held above zero");
        CHECK(ps2_pad_trigger(0.6f, 0.5f, &p) == 0 && p == 0, "trigger inside its dead zone");
        CHECK(ps2_pad_trigger(0.6f, 0.7f, &p) == 1 && p == 178, "trigger past its dead zone (%u)", p);
        CHECK(ps2_pad_button(0.0f, 1.0f) == 1 && ps2_pad_button(0.0f, 0.0f) == 0, "button");
        CHECK(ps2_pad_button(0.5f, 0.4f) == 0 && ps2_pad_button(0.5f, 0.5f) == 1, "button dead zone");
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
