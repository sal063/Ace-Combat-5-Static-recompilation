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

enum { REF_UP, REF_RIGHT, REF_DOWN, REF_LEFT };
typedef struct {
    u8 rawInputs[4];
    u8 lx, ly;
    int lxInvert, lyInvert;
    float axisScale, axisDeadzone;
} ref_pad;

static float ref_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void ref_set(ref_pad *p, int index, float value) {
    p->rawInputs[index] = (u8)ref_clamp(value * p->axisScale * 255.0f, 0.0f, 255.0f);
#define MERGE(pos, neg) ((p->rawInputs[pos] != 0) ? (127u + ((p->rawInputs[pos] + 1u) / 2u)) : (127u - (p->rawInputs[neg] / 2u)))
    p->lx = p->lxInvert ? MERGE(REF_LEFT, REF_RIGHT) : MERGE(REF_RIGHT, REF_LEFT);
    p->ly = p->lyInvert ? MERGE(REF_UP, REF_DOWN) : MERGE(REF_DOWN, REF_UP);
#undef MERGE
    const float dz = p->axisDeadzone;
    if (dz > 0.0f) {
#define MERGE_F(pos, neg) ((p->rawInputs[pos] != 0) ? ((float)p->rawInputs[pos] / 255.0f) : ((float)p->rawInputs[neg] / -255.0f))
        float posX = p->lxInvert ? MERGE_F(REF_LEFT, REF_RIGHT) : MERGE_F(REF_RIGHT, REF_LEFT);
        float posY = p->lyInvert ? MERGE_F(REF_UP, REF_DOWN) : MERGE_F(REF_DOWN, REF_UP);
#undef MERGE_F
        if (posX != 0.0f || posY != 0.0f) {
            const float theta = atan2f(posY, posX);
            const float dzX = cosf(theta) * dz;
            const float dzY = sinf(theta) * dz;
            const int inX = (posX < 0.0f) ? (posX > dzX) : (posX <= dzX);
            const int inY = (posY < 0.0f) ? (posY > dzY) : (posY <= dzY);
            if (inX && inY) p->lx = p->ly = 127;
        }
    }
}

static void ref_axis_event(ref_pad *p, int y_axis, int raw) {
    const float value = (float)raw / (raw < 0 ? 32768.0f : 32767.0f);
    ref_set(p, y_axis ? REF_DOWN : REF_RIGHT, ref_clamp(value > 0.0f ? value : 0.0f, 0.0f, 1.0f));
    ref_set(p, y_axis ? REF_UP : REF_LEFT, ref_clamp(value < 0.0f ? -value : 0.0f, 0.0f, 1.0f));
}

static void our_stick(float dz, float scale, int invert, int x, int y, u8 *ox, u8 *oy) {
    const float v[4] = { ps2_pad_axis_value(y, 0), ps2_pad_axis_value(y, 1),
                         ps2_pad_axis_value(x, 0), ps2_pad_axis_value(x, 1) };
    ps2_pad_stick(dz, scale, invert, v, ox, oy);
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
    CHECK(ps2_cfg.deadzone == 0.0f && ps2_cfg.axis_scale == 1.33f && ps2_cfg.button_deadzone == 0.0f
          && ps2_cfg.invert[0] == 0 && ps2_cfg.invert[1] == 0,
          "analog defaults are not PCSX2's (PadTypes.h:102-106)");

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
        a.invert[0] = 2; a.invert[1] = 3;
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
              && b->invert[0] == 2 && b->invert[1] == 3, "analog fields did not round-trip");
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
              "hand-edited analog values were not clamped to PCSX2's ranges");
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
        CHECK(ps2_cfg.deadzone == 0.0f && ps2_cfg.axis_scale == 1.33f && ps2_cfg.button_deadzone == 0.0f,
              "an old dead zone key changed the PCSX2 settings");
        CHECK(ps2_cfg.invert[0] == 1 && ps2_cfg.invert[1] == 2,
              "old per-stick inverts did not carry over (%d, %d)", ps2_cfg.invert[0], ps2_cfg.invert[1]);
    }

    {
        static const float dzs[] = { 0.0f, 0.1f, 0.25f, 0.5f, 1.0f };
        static const float scales[] = { 0.5f, 1.0f, 1.33f, 2.0f };
        long compared = 0, differ = 0, order = 0;
        for (size_t a = 0; a < sizeof dzs / sizeof dzs[0]; a++)
        for (size_t b = 0; b < sizeof scales / sizeof scales[0]; b++)
        for (int inv = 0; inv < 4; inv++)
        for (int i = 0; i < 256; i++)
        for (int j = 0; j < 256; j++) {
            int x = -32768 + i * 257, y = -32768 + j * 257;
            ref_pad r = { {0}, 127, 127, inv & 1, (inv & 2) != 0, scales[b], dzs[a] };
            u8 ox, oy;
            ref_axis_event(&r, 0, x);
            ref_axis_event(&r, 1, y);
            our_stick(dzs[a], scales[b], inv, x, y, &ox, &oy);
            compared++;
            if (ox != r.lx || oy != r.ly) {
                if (!differ)
                    printf("first difference: dead zone %.2f scale %.2f invert %d at (%d, %d): "
                           "PCSX2 %u,%u, ours %u,%u\n", (double)dzs[a], (double)scales[b], inv,
                           x, y, r.lx, r.ly, ox, oy);
                differ++;
            }
            if ((i + j) % 17 == 0) {
                ref_pad q = { {0}, 127, 127, inv & 1, (inv & 2) != 0, scales[b], dzs[a] };
                ref_axis_event(&q, 1, y);
                ref_axis_event(&q, 0, x);
                if (q.lx != r.lx || q.ly != r.ly) order++;
            }
        }
        printf("stick: %ld positions and settings compared with PCSX2, %ld differ, "
               "%ld depend on event order\n", compared, differ, order);
        CHECK(differ == 0, "the stick model differs from PCSX2's");
        CHECK(order == 0, "PCSX2's result depended on event order");
    }

    {
        u8 x, y, p;
        CHECK(ps2_pad_axis_value(-32768, 0) == 1.0f && ps2_pad_axis_value(32767, 1) == 1.0f
              && ps2_pad_axis_value(-1, 1) == 0.0f && ps2_pad_axis_value(16384, 0) == 0.0f,
              "axis values");
        our_stick(0.0f, 1.33f, 0, 0, 0, &x, &y);
        CHECK(x == 127 && y == 127, "centre is not 0x7F (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 32767, 32767, &x, &y);
        CHECK(x == 255 && y == 255, "right and down (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, -32768, -32768, &x, &y);
        CHECK(x == 0 && y == 0, "left and up (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 0, 16384, 0, &x, &y);
        CHECK(x == 212 && y == 127, "half right at 133%% (%u)", x);
        our_stick(0.0f, 1.33f, 0, 24700, 0, &x, &y);
        CHECK(x == 255, "133%% does not saturate by 75.4%% of travel (%u)", x);
        our_stick(0.25f, 1.0f, 0, 6553, 0, &x, &y);
        CHECK(x == 127 && y == 127, "20%% inside a 25%% dead zone (%u)", x);
        our_stick(0.25f, 1.0f, 0, 9830, 0, &x, &y);
        CHECK(x == 165, "30%% outside a 25%% dead zone is not passed on unscaled (%u)", x);
        our_stick(0.25f, 1.0f, 0, 6553, 6553, &x, &y);
        CHECK(x == 152 && y == 152, "the dead zone is not radial (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 1, 32767, 32767, &x, &y);
        CHECK(x == 0 && y == 255, "invert left/right (%u, %u)", x, y);
        our_stick(0.0f, 1.33f, 2, 32767, 32767, &x, &y);
        CHECK(x == 255 && y == 0, "invert up/down (%u, %u)", x, y);
        {
            const float both[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            ps2_pad_stick(0.0f, 1.33f, 0, both, &x, &y);
            CHECK(x == 255 && y == 255, "opposing directions (%u, %u)", x, y);
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
