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
    u32 emitter;
    int native_region;
} ps2_vk_state;

enum { PS2_VK_POINTS = 0, PS2_VK_LINES = 1, PS2_VK_TRIANGLES = 2 };

int  ps2_vk_enabled(void);
int  ps2_vk_closed(void);

void ps2_vk_draw(int topology, const ps2_vk_state *st,
                 const ps2_vk_vertex *v, int n);

enum { PS2_VK_NATIVE_NONE = 0, PS2_VK_NATIVE_SUN_FLARE = 1,
       PS2_VK_NATIVE_SUN_GHOST = 2,
       PS2_VK_NATIVE_2D = 3 };

void ps2_vk_draw_2d(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n);
typedef struct ps2_vk_native {
    u32 pass;
    u32 rt;
    u32 zrt;
    u32 tex;
    float tex_w, tex_h;
    int tex_point;
    u32 wrap;
    float box[4];
    float sun_z;
    s32 scissor[4];
} ps2_vk_native;

void ps2_vk_native_draw(const ps2_vk_native *nd, const ps2_vk_vertex *v, int n);

typedef struct ps2_vk_mesh_block {
    float m[16];
    float c[16];
    float l[12];
    float r[16];
    float e[12];
    float fog0, fog1;
    float zscale, zmax;
    float xoff, yoff;
    float spec_alpha;
    float pad0;
    u32 mode;
    u32 pass;
    u32 clip;
    u32 pad1;
    u32 prog;
    u32 pad2, pad3, pad4;
    float p[16];
} ps2_vk_mesh_block;

enum { PS2_VK_NATIVE_MESH = 4 };

int  ps2_vk_mesh_available(void);
int  ps2_vk_mesh_material_ok(const ps2_vk_state *st);
int  ps2_vk_mesh_store(const u8 *records, u32 n_verts, const u32 *indices,
                       u32 n_indices, u32 *vbase, u32 *ifirst);
int  ps2_vk_mesh_room(u32 n_verts, u32 n_indices, u32 n_draws);
enum { PS2_VK_MESH_CULL = 1, PS2_VK_MESH_GSFRAG = 4 };
void ps2_vk_draw_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                      u32 vbase, u32 ifirst, u32 n_indices, int cull);
void ps2_vk_draw_mesh_topo(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                           u32 vbase, u32 ifirst, u32 n_indices, int cull,
                           int topology);
extern void (*ps2_vk_before_record)(void);

u32  ps2_vk_texture(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
int  ps2_vk_texture_is(u32 idx, u64 key, u32 hash);

u32  ps2_vk_texture_native(u32 src, u32 x, u32 y, u32 w, u32 h);
u32  ps2_vk_texture_native_find(u64 key, u32 hash, u32 w, u32 h);
u32  ps2_vk_texture_native_rgba(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
const u8 *ps2_vk_texture_pixels(u32 idx);
void ps2_vk_texture_size(u32 idx, u32 *w, u32 *h);

void ps2_vk_invalidate(u32 base, u32 size);

#define PS2_VK_PRESENT_BLANK 0xFFFFFFFFu
int  ps2_vk_present(u32 disp_x, u32 disp_y, u32 disp_w, u32 disp_h, u32 rt);

extern int ps2_vk_lockstep;

int  ps2_vk_screenshot(const char *path);

extern float ps2_vk_display_aspect;

void ps2_vk_stats(u64 *frames, u64 *draws, u64 *verts, u64 *textures);

#ifdef __cplusplus
}
#endif

#endif
