#ifndef PS2_VK_H
#define PS2_VK_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_vk_vertex {
    float x, y, z;
    float r, g, b, a;
    float s, t, q;
    float fog;
    u32 round_uv;
} ps2_vk_vertex;

#define PS2_VK_RUV_SPRITE (1u << 29)

typedef struct ps2_vk_state {
    u32 fb_w, fb_h;
    u32 fb_psm;
    u32 fb_mask;
    int shuffle_rg;
    u32 shuffle_alpha;
    s32 scissor[4];

    u32 ztst;
    int zwrite;

    int abe;
    u32 alpha_a, alpha_b, alpha_c, alpha_d, alpha_fix;

    int tme;
    u32 tex_index;
    float tex_w, tex_h;
    u32 tfx;
    int tcc;

    u32 rt;
    u32 tex_rt;
    int tex_depth;
    int tex_self;
    int fst;
    u32 tex_lod;
    int tex_point;

    u32 wms, wmt;
    u32 minu, maxu, minv, maxv;

    int ate;
    u32 atst, aref;
    u32 afail;
    int zte;

    int fge;
    u32 fogcol;
    int fba;
    int date, datm;
    u32 zrt;
    int zcopy;
    int colclip;
} ps2_vk_state;

enum { PS2_VK_POINTS = 0, PS2_VK_LINES = 1, PS2_VK_TRIANGLES = 2 };

int  ps2_vk_enabled(void);
int  ps2_vk_closed(void);

void ps2_vk_draw(int topology, const ps2_vk_state *st,
                 const ps2_vk_vertex *v, int n);

u32  ps2_vk_texture(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);

void ps2_vk_invalidate(u32 base, u32 size);

int  ps2_vk_present(u32 disp_x, u32 disp_y, u32 disp_w, u32 disp_h, u32 rt);

extern int ps2_vk_lockstep;

int  ps2_vk_screenshot(const char *path);

void ps2_vk_stats(u64 *frames, u64 *draws, u64 *verts, u64 *textures);

#ifdef __cplusplus
}
#endif

#endif
