#ifndef PS2_SETTINGS_H
#define PS2_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS2_ACT_SELECT = 0, PS2_ACT_L3, PS2_ACT_R3, PS2_ACT_START,
    PS2_ACT_UP, PS2_ACT_RIGHT, PS2_ACT_DOWN, PS2_ACT_LEFT,
    PS2_ACT_L2, PS2_ACT_R2, PS2_ACT_L1, PS2_ACT_R1,
    PS2_ACT_TRIANGLE, PS2_ACT_CIRCLE, PS2_ACT_CROSS, PS2_ACT_SQUARE,
    PS2_ACT_LS_UP, PS2_ACT_LS_DOWN, PS2_ACT_LS_LEFT, PS2_ACT_LS_RIGHT,
    PS2_ACT_RS_UP, PS2_ACT_RS_DOWN, PS2_ACT_RS_LEFT, PS2_ACT_RS_RIGHT,
    PS2_ACT_COUNT
};
#define PS2_ACT_BUTTONS 16
#define PS2_BIND_SLOTS  2

#define PS2_PADBIND_AXIS          0x100
#define PS2_PADBIND_BUTTON(b)     ((int)(b) + 1)
#define PS2_PADBIND_AXISDIR(a, p) (PS2_PADBIND_AXIS | ((int)(a) << 1) | ((p) ? 1 : 0))

enum { PS2_PRESET_LOW, PS2_PRESET_MEDIUM, PS2_PRESET_HIGH, PS2_PRESET_ULTRA,
       PS2_PRESET_CUSTOM };
enum { PS2_TEXFILTER_GAME, PS2_TEXFILTER_NEAREST, PS2_TEXFILTER_BILINEAR };
enum { PS2_SCALE_NEAREST, PS2_SCALE_BILINEAR, PS2_SCALE_SHARP };
enum { PS2_WIN_WINDOWED, PS2_WIN_BORDERLESS, PS2_WIN_EXCLUSIVE };
enum { PS2_ASPECT_STRETCH, PS2_ASPECT_AUTO, PS2_ASPECT_4_3, PS2_ASPECT_16_9,
       PS2_ASPECT_CUSTOM };
enum { PS2_PRESENT_MAILBOX, PS2_PRESENT_FIFO, PS2_PRESENT_IMMEDIATE,
       PS2_PRESENT_FIFO_RELAXED };
enum { PS2_DZ_AXIAL, PS2_DZ_RADIAL, PS2_DZ_SCALED_RADIAL };

typedef struct {
    int   shape;
    float inner;
    float outer;
    float curve;
    int   invert_x, invert_y;
} ps2_stick_cfg;

#define PS2_INTERNAL_RES_MAX 8

typedef struct ps2_settings {
    int   preset;
    int   internal_res;
    int   tex_filter;
    int   anisotropy;
    int   deinterlace;
    int   fxaa;
    float sharpen;
    int   scale_filter;
    float brightness;
    float contrast;
    float gamma;
    float saturation;
    int   window_mode;
    int   fullscreen_type;
    int   window_w, window_h;
    int   fs_w, fs_h;
    float fs_hz;
    int   aspect;
    float aspect_custom;
    int   integer_scale;
    int   widescreen;
    int   present_mode;
    int   fps_limit;
    int   show_fps;
    float ui_scale;
    int   block_input_in_menu;
    int   key[PS2_ACT_COUNT][PS2_BIND_SLOTS];
    int   pad[PS2_ACT_COUNT][PS2_BIND_SLOTS];
    ps2_stick_cfg stick[2];
    float trigger_deadzone;
    float trigger_press;
    float axis_press;
} ps2_settings;

extern ps2_settings ps2_cfg;

enum { PS2_CFG_SWAPCHAIN = 1, PS2_CFG_WINDOW = 2, PS2_CFG_SAMPLER = 4,
       PS2_CFG_FPS = 8, PS2_CFG_SAVE = 16 };

void ps2_settings_defaults(ps2_settings *s);
void ps2_settings_default_bindings(ps2_settings *s, int keyboard, int pad);
void ps2_settings_apply_preset(ps2_settings *s, int preset);
int  ps2_settings_preset_matches(const ps2_settings *s);
void ps2_settings_load(void);
void ps2_settings_save(void);
int  ps2_settings_save_pending(void);
void ps2_settings_disable_file(void);
const char *ps2_settings_path(void);
void ps2_settings_touch(int dirty);
int  ps2_settings_take_dirty(void);

const char *ps2_action_name(int act);
const char *ps2_key_name(int scancode);
const char *ps2_padbind_name(int code);

void ps2_settings_apply_game_patches(void);

void ps2_stick_process(const ps2_stick_cfg *c, float x, float y,
                       float *ox, float *oy);

#ifdef __cplusplus
}
#endif

#endif
