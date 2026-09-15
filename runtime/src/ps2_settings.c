#include "ps2_runtime.h"
#include "ps2_settings.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ps2_settings ps2_cfg;

static atomic_int cfg_dirty;
static atomic_int cfg_save_pending;
static int cfg_file_disabled;
static char cfg_path[1024];

static const char *const act_names[PS2_ACT_COUNT] = {
    "Select", "L3", "R3", "Start",
    "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left",
    "L2", "R2", "L1", "R1",
    "Triangle", "Circle", "Cross", "Square",
    "Left stick Up", "Left stick Down", "Left stick Left", "Left stick Right",
    "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
};

static const char *const act_keys[PS2_ACT_COUNT] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left",
    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right",
};

static const char *const pad_button_names[] = {
    "South (A / Cross)", "East (B / Circle)", "West (X / Square)",
    "North (Y / Triangle)", "Back / Select", "Guide", "Start",
    "Left stick click", "Right stick click", "Left bumper", "Right bumper",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Misc",
    "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2",
    "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6",
};

const char *ps2_action_name(int act) {
    return act >= 0 && act < PS2_ACT_COUNT ? act_names[act] : "?";
}

const char *ps2_key_name(int scancode) {
    const char *n;
    if (scancode <= 0) return "-";
    n = SDL_GetScancodeName((SDL_Scancode)scancode);
    return n && *n ? n : "Unknown key";
}

const char *ps2_padbind_name(int code) {
    static char buf[4][48];
    static unsigned next;
    char *out = buf[next++ % 4];
    if (code <= 0) return "-";
    if (code & PS2_PADBIND_AXIS) {
        int axis = (code & 0xFF) >> 1, pos = code & 1;
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:  return pos ? "Left stick Right" : "Left stick Left";
        case SDL_GAMEPAD_AXIS_LEFTY:  return pos ? "Left stick Down" : "Left stick Up";
        case SDL_GAMEPAD_AXIS_RIGHTX: return pos ? "Right stick Right" : "Right stick Left";
        case SDL_GAMEPAD_AXIS_RIGHTY: return pos ? "Right stick Down" : "Right stick Up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return "Left trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right trigger";
        default: snprintf(out, 48, "Axis %d%c", axis, pos ? '+' : '-'); return out;
        }
    }
    if (code - 1 < (int)(sizeof pad_button_names / sizeof pad_button_names[0]))
        return pad_button_names[code - 1];
    snprintf(out, 48, "Button %d", code - 1);
    return out;
}

void ps2_settings_default_bindings(ps2_settings *s, int keyboard, int pad) {
    if (keyboard) {
        memset(s->key, 0, sizeof s->key);
        s->key[PS2_ACT_START][0]    = SDL_SCANCODE_RETURN;
        s->key[PS2_ACT_SELECT][0]   = SDL_SCANCODE_RSHIFT;
        s->key[PS2_ACT_UP][0]       = SDL_SCANCODE_UP;
        s->key[PS2_ACT_DOWN][0]     = SDL_SCANCODE_DOWN;
        s->key[PS2_ACT_LEFT][0]     = SDL_SCANCODE_LEFT;
        s->key[PS2_ACT_RIGHT][0]    = SDL_SCANCODE_RIGHT;
        s->key[PS2_ACT_CROSS][0]    = SDL_SCANCODE_X;
        s->key[PS2_ACT_SQUARE][0]   = SDL_SCANCODE_Z;
        s->key[PS2_ACT_CIRCLE][0]   = SDL_SCANCODE_S;
        s->key[PS2_ACT_TRIANGLE][0] = SDL_SCANCODE_A;
        s->key[PS2_ACT_L1][0]       = SDL_SCANCODE_Q;
        s->key[PS2_ACT_R1][0]       = SDL_SCANCODE_E;
        s->key[PS2_ACT_L2][0]       = SDL_SCANCODE_1;
        s->key[PS2_ACT_R2][0]       = SDL_SCANCODE_3;
        s->key[PS2_ACT_LS_UP][0]    = SDL_SCANCODE_W;
        s->key[PS2_ACT_LS_UP][1]    = SDL_SCANCODE_KP_8;
        s->key[PS2_ACT_LS_DOWN][0]  = SDL_SCANCODE_KP_2;
        s->key[PS2_ACT_LS_LEFT][0]  = SDL_SCANCODE_KP_4;
        s->key[PS2_ACT_LS_RIGHT][0] = SDL_SCANCODE_KP_6;
    }
    if (pad) {
        memset(s->pad, 0, sizeof s->pad);
        s->pad[PS2_ACT_CROSS][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_SOUTH);
        s->pad[PS2_ACT_CIRCLE][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_EAST);
        s->pad[PS2_ACT_SQUARE][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_WEST);
        s->pad[PS2_ACT_TRIANGLE][0] = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_NORTH);
        s->pad[PS2_ACT_SELECT][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_BACK);
        s->pad[PS2_ACT_START][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_START);
        s->pad[PS2_ACT_L3][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_STICK);
        s->pad[PS2_ACT_R3][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        s->pad[PS2_ACT_L1][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        s->pad[PS2_ACT_R1][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        s->pad[PS2_ACT_UP][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_UP);
        s->pad[PS2_ACT_DOWN][0]     = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        s->pad[PS2_ACT_LEFT][0]     = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        s->pad[PS2_ACT_RIGHT][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        s->pad[PS2_ACT_L2][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
        s->pad[PS2_ACT_R2][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
        s->pad[PS2_ACT_LS_UP][0]    = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTY, 0);
        s->pad[PS2_ACT_LS_DOWN][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTY, 1);
        s->pad[PS2_ACT_LS_LEFT][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTX, 0);
        s->pad[PS2_ACT_LS_RIGHT][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTX, 1);
        s->pad[PS2_ACT_RS_UP][0]    = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTY, 0);
        s->pad[PS2_ACT_RS_DOWN][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTY, 1);
        s->pad[PS2_ACT_RS_LEFT][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTX, 0);
        s->pad[PS2_ACT_RS_RIGHT][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTX, 1);
    }
}

typedef struct {
    int internal_res, tex_filter, anisotropy, deinterlace, fxaa, scale_filter;
    float sharpen;
} preset_def;

static const preset_def presets[4] = {
    { 1, PS2_TEXFILTER_GAME,  0, 1, 0, PS2_SCALE_BILINEAR, 0.00f },
    { 2, PS2_TEXFILTER_GAME,  4, 1, 0, PS2_SCALE_BILINEAR, 0.00f },
    { 3, PS2_TEXFILTER_GAME,  8, 1, 0, PS2_SCALE_SHARP,    0.20f },
    { 4, PS2_TEXFILTER_GAME, 16, 1, 0, PS2_SCALE_SHARP,    0.20f },
};

void ps2_settings_apply_preset(ps2_settings *s, int preset) {
    const preset_def *p;
    if (preset < 0 || preset > PS2_PRESET_ULTRA) return;
    p = &presets[preset];
    s->preset       = preset;
    s->internal_res = p->internal_res;
    s->tex_filter   = p->tex_filter;
    s->anisotropy   = p->anisotropy;
    s->deinterlace  = p->deinterlace;
    s->fxaa         = p->fxaa;
    s->scale_filter = p->scale_filter;
    s->sharpen      = p->sharpen;
}

int ps2_settings_preset_matches(const ps2_settings *s) {
    for (int i = 0; i <= PS2_PRESET_ULTRA; i++) {
        const preset_def *p = &presets[i];
        if (s->internal_res == p->internal_res
            && s->tex_filter == p->tex_filter && s->anisotropy == p->anisotropy
            && s->deinterlace == p->deinterlace
            && s->fxaa == p->fxaa && s->scale_filter == p->scale_filter
            && fabsf(s->sharpen - p->sharpen) < 0.005f)
            return i;
    }
    return PS2_PRESET_CUSTOM;
}

void ps2_settings_defaults(ps2_settings *s) {
    memset(s, 0, sizeof *s);
    ps2_settings_apply_preset(s, PS2_PRESET_LOW);
    s->brightness = 0.0f;
    s->contrast = 1.0f;
    s->gamma = 1.0f;
    s->saturation = 1.0f;
    s->window_mode = PS2_WIN_WINDOWED;
    s->fullscreen_type = PS2_WIN_BORDERLESS;
    s->window_w = 1280;
    s->window_h = 896;
    s->aspect = PS2_ASPECT_STRETCH;
    s->aspect_custom = 16.0f / 9.0f;
    s->present_mode = PS2_PRESENT_MAILBOX;
    s->ui_scale = 1.0f;
    s->block_input_in_menu = 1;
    ps2_settings_default_bindings(s, 1, 1);
    s->deadzone = 0.0f;
    s->axis_scale = 1.33f;
    s->button_deadzone = 0.0f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void sanitize(ps2_settings *s) {
    s->internal_res = clampi(s->internal_res, 0, PS2_INTERNAL_RES_MAX);
    s->tex_filter = clampi(s->tex_filter, 0, 2);
    if (s->anisotropy != 0 && s->anisotropy != 2 && s->anisotropy != 4
        && s->anisotropy != 8 && s->anisotropy != 16)
        s->anisotropy = 0;
    s->deinterlace = s->deinterlace != 0;
    s->fxaa = s->fxaa != 0;
    s->sharpen = clampf(s->sharpen, 0.0f, 1.0f);
    s->scale_filter = clampi(s->scale_filter, 0, 2);
    s->brightness = clampf(s->brightness, -0.5f, 0.5f);
    s->contrast = clampf(s->contrast, 0.5f, 1.5f);
    s->gamma = clampf(s->gamma, 0.5f, 2.0f);
    s->saturation = clampf(s->saturation, 0.0f, 2.0f);
    s->window_mode = clampi(s->window_mode, 0, 2);
    if (s->fullscreen_type != PS2_WIN_EXCLUSIVE) s->fullscreen_type = PS2_WIN_BORDERLESS;
    s->window_w = clampi(s->window_w, 320, 16384);
    s->window_h = clampi(s->window_h, 224, 16384);
    s->fs_w = clampi(s->fs_w, 0, 16384);
    s->fs_h = clampi(s->fs_h, 0, 16384);
    s->fs_hz = clampf(s->fs_hz, 0.0f, 1000.0f);
    s->aspect = clampi(s->aspect, 0, 4);
    s->aspect_custom = clampf(s->aspect_custom, 0.5f, 4.0f);
    s->integer_scale = s->integer_scale != 0;
    s->widescreen = s->widescreen != 0;
    s->present_mode = clampi(s->present_mode, 0, 3);
    s->fps_limit = clampi(s->fps_limit, 0, 1000);
    s->show_fps = s->show_fps != 0;
    s->ui_scale = clampf(s->ui_scale, 0.5f, 3.0f);
    s->block_input_in_menu = s->block_input_in_menu != 0;
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        for (int k = 0; k < PS2_BIND_SLOTS; k++) {
            if (s->key[a][k] < 0 || s->key[a][k] >= SDL_SCANCODE_COUNT) s->key[a][k] = 0;
            if (s->pad[a][k] < 0 || s->pad[a][k] > 0x1FF) s->pad[a][k] = 0;
        }
    s->deadzone = clampf(s->deadzone, 0.0f, 1.0f);
    s->axis_scale = clampf(s->axis_scale, 0.01f, 2.0f);
    s->button_deadzone = clampf(s->button_deadzone, 0.0f, 1.0f);
    s->invert[0] = clampi(s->invert[0], 0, 3);
    s->invert[1] = clampi(s->invert[1], 0, 3);
}

typedef struct {
    const char *section, *key;
    char type;
    size_t off;
    const char *comment;
} cfg_field;

#define FI(sec, k, m, c) { sec, k, 'i', offsetof(ps2_settings, m), c }
#define FF(sec, k, m, c) { sec, k, 'f', offsetof(ps2_settings, m), c }

static const cfg_field fields[] = {
    FI("graphics", "preset", preset, "0 low, 1 medium, 2 high, 3 ultra, 4 custom"),
    FI("graphics", "internal_resolution", internal_res, "0 auto (cover the window), 1 native 640x448, 2..8 times that"),
    FI("graphics", "texture_filter", tex_filter, "0 as the game asks, 1 force nearest, 2 force bilinear"),
    FI("graphics", "anisotropy", anisotropy, "0 off, 2, 4, 8 or 16"),
    FI("graphics", "deinterlace", deinterlace, "1 weave the two fields into whole frames"),
    FI("graphics", "scale_filter", scale_filter, "0 nearest, 1 bilinear, 2 sharp bilinear"),
    FI("postfx", "fxaa", fxaa, "1 on"),
    FF("postfx", "sharpen", sharpen, "0..1"),
    FF("postfx", "brightness", brightness, "-0.5..0.5"),
    FF("postfx", "contrast", contrast, "0.5..1.5"),
    FF("postfx", "gamma", gamma, "0.5..2.0"),
    FF("postfx", "saturation", saturation, "0..2"),
    FI("display", "window_mode", window_mode, "0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen"),
    FI("display", "fullscreen_type", fullscreen_type, "what F11 enters: 1 borderless, 2 exclusive"),
    FI("display", "window_width", window_w, "windowed size in pixels"),
    FI("display", "window_height", window_h, ""),
    FI("display", "fullscreen_width", fs_w, "exclusive fullscreen; 0 = desktop resolution"),
    FI("display", "fullscreen_height", fs_h, ""),
    FF("display", "fullscreen_refresh", fs_hz, "Hz; 0 = desktop refresh rate"),
    FI("display", "aspect", aspect, "0 stretch to window, 1 auto (4:3, 16:9 with widescreen), 2 4:3, 3 16:9, 4 custom"),
    FF("display", "aspect_custom", aspect_custom, "width / height"),
    FI("display", "integer_scale", integer_scale, "1 whole-number scaling only"),
    FI("display", "widescreen", widescreen, "1 apply the 16:9 game patch"),
    FI("display", "present_mode", present_mode, "0 mailbox, 1 vsync (fifo), 2 immediate, 3 adaptive vsync"),
    FI("display", "fps_limit", fps_limit, "0 off"),
    FI("display", "show_fps", show_fps, "1 on"),
    FF("display", "menu_scale", ui_scale, "0.5..3"),
    FI("input", "block_game_input_in_menu", block_input_in_menu, "1 on"),
    FF("analog", "deadzone", deadzone, "PCSX2 Analog Deadzone, both sticks, 0..1"),
    FF("analog", "axis_scale", axis_scale, "PCSX2 Analog Sensitivity, 0.01..2 (default 1.33)"),
    FF("analog", "button_deadzone", button_deadzone, "PCSX2 Button/Trigger Deadzone, 0..1"),
    FI("analog", "invert_left", invert[0], "0 none, 1 left/right, 2 up/down, 3 both"),
    FI("analog", "invert_right", invert[1], "0 none, 1 left/right, 2 up/down, 3 both"),
};

void ps2_settings_disable_file(void) {
    const char *e = getenv("PS2_SETTINGS_FILE");
    cfg_file_disabled = !(e && *e);
}

const char *ps2_settings_path(void) {
    if (!cfg_path[0]) {
        const char *e = getenv("PS2_SETTINGS_FILE");
        const char *base = SDL_GetBasePath();
        if (e && *e) snprintf(cfg_path, sizeof cfg_path, "%s", e);
        else snprintf(cfg_path, sizeof cfg_path, "%sac5_settings.ini", base ? base : "");
    }
    return cfg_path;
}

static char *trim(char *p) {
    char *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return p;
}

static void log_summary(const char *source) {
    static const char *const presets_n[] = { "low", "medium", "high", "ultra", "custom" };
    static const char *const win_n[] = { "windowed", "borderless", "exclusive" };
    static const char *const present_n[] = { "mailbox", "fifo", "immediate", "fifo-relaxed" };
    ps2_log("settings: %s -- preset %s, internal resolution %s%d, texture filter %d, "
            "anisotropy %d, %s %dx%d, aspect %d, widescreen %d, present %s, fps limit %d",
            source, presets_n[ps2_cfg.preset], ps2_cfg.internal_res ? "" : "auto ",
            ps2_cfg.internal_res, ps2_cfg.tex_filter, ps2_cfg.anisotropy,
            win_n[ps2_cfg.window_mode], ps2_cfg.window_w, ps2_cfg.window_h, ps2_cfg.aspect,
            ps2_cfg.widescreen, present_n[ps2_cfg.present_mode], ps2_cfg.fps_limit);
}

void ps2_settings_load(void) {
    char line[512], section[64] = "";
    FILE *f;
    ps2_settings_defaults(&ps2_cfg);
    if (cfg_file_disabled) return;
    f = fopen(ps2_settings_path(), "r");
    if (!f) {
        log_summary("no settings file, defaults");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p, *eq, *key, *val, *c;
        c = strpbrk(line, ";#");
        if (c) *c = 0;
        p = trim(line);
        if (!*p) continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", p + 1); }
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(p);
        val = trim(eq + 1);
        if (!strcmp(section, "keyboard") || !strcmp(section, "controller")) {
            int act, a = 0, b = 0;
            for (act = 0; act < PS2_ACT_COUNT; act++)
                if (!strcmp(key, act_keys[act])) break;
            if (act == PS2_ACT_COUNT) continue;
            if (sscanf(val, "%d %d", &a, &b) < 1) continue;
            if (section[0] == 'k') { ps2_cfg.key[act][0] = a; ps2_cfg.key[act][1] = b; }
            else                   { ps2_cfg.pad[act][0] = a; ps2_cfg.pad[act][1] = b; }
            continue;
        }
        if (!strcmp(section, "analog")) {
            static const char *const old_invert[4] = {
                "left_invert_x", "left_invert_y", "right_invert_x", "right_invert_y",
            };
            int i = 0;
            while (i < 4 && strcmp(key, old_invert[i])) i++;
            if (i < 4) {
                if (atoi(val)) ps2_cfg.invert[i / 2] |= 1 << (i % 2);
                continue;
            }
        }
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            if (strcmp(fields[i].section, section) || strcmp(fields[i].key, key)) continue;
            if (fields[i].type == 'i') *(int *)((char *)&ps2_cfg + fields[i].off) = atoi(val);
            else *(float *)((char *)&ps2_cfg + fields[i].off) = strtof(val, NULL);
            break;
        }
    }
    fclose(f);
    sanitize(&ps2_cfg);
    ps2_cfg.preset = ps2_settings_preset_matches(&ps2_cfg);
    ps2_log("settings: loaded %s", ps2_settings_path());
    log_summary("in use");
}

void ps2_settings_save(void) {
    char tmp[1100];
    const char *path;
    const char *last = "";
    FILE *f;
    if (cfg_file_disabled) return;
    atomic_store(&cfg_save_pending, 0);
    path = ps2_settings_path();
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) { ps2_log("settings: cannot write %s", tmp); return; }
    fprintf(f, "# Ace Combat 5 recompiled -- settings.\n"
               "# Written by the in-game menu (F4).  Hand edits are kept as long as\n"
               "# each line stays key = value; unknown keys are ignored.\n");
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const cfg_field *fd = &fields[i];
        char val[32];
        if (strcmp(last, fd->section)) {
            fprintf(f, "\n[%s]\n", fd->section);
            last = fd->section;
        }
        if (fd->type == 'i') snprintf(val, sizeof val, "%d", *(int *)((char *)&ps2_cfg + fd->off));
        else snprintf(val, sizeof val, "%.6f", (double)*(float *)((char *)&ps2_cfg + fd->off));
        if (fd->comment[0]) fprintf(f, "%-26s = %-10s ; %s\n", fd->key, val, fd->comment);
        else fprintf(f, "%-26s = %s\n", fd->key, val);
    }
    fprintf(f, "\n[keyboard]\n; action = primary secondary, as SDL scancodes (0 = unbound)\n");
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        fprintf(f, "%-26s = %3d %3d    ; %s, %s\n", act_keys[a], ps2_cfg.key[a][0],
                ps2_cfg.key[a][1], ps2_key_name(ps2_cfg.key[a][0]),
                ps2_key_name(ps2_cfg.key[a][1]));
    fprintf(f, "\n[controller]\n; action = primary secondary: 1 + SDL button, or 256 | axis << 1 | positive\n");
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        fprintf(f, "%-26s = %3d %3d    ; %s, %s\n", act_keys[a], ps2_cfg.pad[a][0],
                ps2_cfg.pad[a][1], ps2_padbind_name(ps2_cfg.pad[a][0]),
                ps2_padbind_name(ps2_cfg.pad[a][1]));
    if (fclose(f) != 0) { ps2_log("settings: write to %s failed", tmp); return; }
    remove(path);
    if (rename(tmp, path) != 0) ps2_log("settings: cannot replace %s", path);
}

int ps2_settings_save_pending(void) { return atomic_load(&cfg_save_pending); }

void ps2_settings_touch(int dirty) {
    if (dirty & PS2_CFG_SAVE) atomic_store(&cfg_save_pending, 1);
    atomic_fetch_or(&cfg_dirty, dirty);
}

int ps2_settings_take_dirty(void) { return atomic_exchange(&cfg_dirty, 0); }

static const u32 ws_addr[2]  = { 0x00440828u, 0x0044082Cu };
static const u32 ws_orig[2]  = { 0x440315C2u, 0x441013D7u };
static const u32 ws_patch[2] = { 0x43D638F3u, 0x43EB7385u };

void ps2_settings_apply_game_patches(void) {
    static int logged = -1;
    if (!ps2_ram) return;
    for (int i = 0; i < 2; i++) {
        u32 cur = ps2_r32(ws_addr[i]);
        if (ps2_cfg.widescreen) {
            if (cur == ws_orig[i]) ps2_w32(ws_addr[i], ws_patch[i]);
        } else if (cur == ws_patch[i]) {
            ps2_w32(ws_addr[i], ws_orig[i]);
        }
    }
    if (logged != ps2_cfg.widescreen) {
        logged = ps2_cfg.widescreen;
        ps2_log("settings: widescreen patch %s", logged ? "on" : "off");
    }
}

float ps2_pad_axis_value(int raw, int positive) {
    float v = (float)raw / (raw < 0 ? 32768.0f : 32767.0f);
    return clampf(positive ? v : -v, 0.0f, 1.0f);
}

void ps2_pad_stick(float deadzone, float axis_scale, int invert, const float v[4],
                   unsigned char *x, unsigned char *y) {
    unsigned raw[4], px, nx, py, ny;
    for (int i = 0; i < 4; i++)
        raw[i] = (unsigned char)clampf(v[i] * axis_scale * 255.0f, 0.0f, 255.0f);
    px = raw[invert & 1 ? 2 : 3];
    nx = raw[invert & 1 ? 3 : 2];
    py = raw[invert & 2 ? 0 : 1];
    ny = raw[invert & 2 ? 1 : 0];
#define MERGE(pos, neg) ((pos) != 0 ? 127u + ((pos) + 1u) / 2u : 127u - (neg) / 2u)
    *x = (unsigned char)MERGE(px, nx);
    *y = (unsigned char)MERGE(py, ny);
#undef MERGE
    if (deadzone > 0.0f) {
        float fx = px != 0 ? (float)px / 255.0f : (float)nx / -255.0f;
        float fy = py != 0 ? (float)py / 255.0f : (float)ny / -255.0f;
        if (fx != 0.0f || fy != 0.0f) {
            float theta = atan2f(fy, fx);
            float dzx = cosf(theta) * deadzone, dzy = sinf(theta) * deadzone;
            int in_x = fx < 0.0f ? fx > dzx : fx <= dzx;
            int in_y = fy < 0.0f ? fy > dzy : fy <= dzy;
            if (in_x && in_y) *x = *y = 127;
        }
    }
}

int ps2_pad_trigger(float button_deadzone, float value, unsigned char *pressure) {
    float s = clampf(value, 0.0f, 1.0f);
    float d = button_deadzone > 0.0f && s < button_deadzone ? 0.0f : s;
    *pressure = (unsigned char)(d * 255.0f);
    return d > 0.0f;
}

int ps2_pad_button(float button_deadzone, float value) {
    return (value < button_deadzone ? 0.0f : value) > 0.0f;
}
