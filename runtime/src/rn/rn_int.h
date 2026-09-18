#ifndef RN_INT_H
#define RN_INT_H

#include "ps2_runtime.h"
#include "ps2_vk.h"
#include "rn.h"

#include <string.h>

extern u32 ps2_gs_frame_count;
void ps2_gs_write_reg(u32 reg, u64 val);
u64  ps2_gs_cur_tex0(void);
u64  ps2_gs_cur_frame(void);
u64  ps2_gs_cur_zbuf(void);
u64  ps2_gs_cur_test(void);
int  ps2_gs_decode_indexed(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32 cbp,
                           u32 cpsm, u8 *rgba);
void ps2_gs_native_vertex(float x, float y, double z, float fog, int kind, int adc);
void ps2_gs_native_state(ps2_vk_state *st);
void ps2_gs_native_frame(float *xoff, float *yoff, float *zmax);
void ps2_gs_native_tex_report(void);

rn_emitter *rn_emitter_mut(u32 id);
u32  rn_tap_open_emitter(u32 pkt);
void rn_intent_frame_end(void);
void rn_intent_tag(u32 tadr, u32 emitter);

extern int rn_sun_on, rn_sky_on, rn_tri3d_on, rn_billboard_on, rn_worldtri_on;

void rn_sun_init(void);
int  rn_sun_claim_prim(const struct ps2_vk_state *st, int kind,
                       const struct ps2_vk_vertex *v, int n);

void rn_sky_init(void);
void rn_sky_intent(const u8 *rec, u32 len);
void rn_sky_end(void);
int  rn_sky_active(void);
int  rn_sky_claim_prim(const struct ps2_vk_state *st, int kind,
                       const struct ps2_vk_vertex *v, int n);
void rn_sky_report(void);

void rn_tri3d_init(void);
void rn_tri3d_intent(const u8 *rec, u32 len);
void rn_tri3d_end(void);
int  rn_tri3d_active(void);
int  rn_tri3d_claim_prim(const struct ps2_vk_state *st, int kind,
                         const struct ps2_vk_vertex *v, int n);
void rn_tri3d_report(void);

void rn_billboard_init(void);
void rn_billboard_intent(const u8 *rec, u32 len);
void rn_billboard_end(void);
int  rn_billboard_active(void);
int  rn_billboard_claim_prim(const struct ps2_vk_state *st, int kind,
                             const struct ps2_vk_vertex *v, int n);
void rn_billboard_report(void);

void rn_worldtri_init(void);
void rn_worldtri_intent(const u8 *rec, u32 len);
void rn_worldtri_end(void);
int  rn_worldtri_active(void);
int  rn_worldtri_claim_prim(const struct ps2_vk_state *st, int kind,
                            const struct ps2_vk_vertex *v, int n);
void rn_worldtri_report(void);

void rn_2d_report(void);

extern int rn_screen_on;
void rn_screen_init(void);
int  rn_screen_claim_prim(const struct ps2_vk_state *st, int kind,
                          const struct ps2_vk_vertex *v, int n);
int  rn_screen_claim_any(const struct ps2_vk_state *st, int kind,
                         const struct ps2_vk_vertex *v, int n);
void rn_screen_flush(void);
void rn_screen_report(void);

static inline void rn_mesh_rec(u8 *rec, const float pos[4], const s32 nrm[4],
                               const s32 col[4], const float uv[4]) {
    memset(rec, 0, 64);
    memcpy(rec, pos, 16);
    if (nrm) memcpy(rec + 16, nrm, 16);
    memcpy(rec + 32, col, 16);
    if (uv) memcpy(rec + 48, uv, 16);
}

static inline void rn_rgba_lanes(u32 rgba, s32 col[4]) {
    col[0] = (s32)(rgba & 0xFFu);
    col[1] = (s32)((rgba >> 8) & 0xFFu);
    col[2] = (s32)((rgba >> 16) & 0xFFu);
    col[3] = (s32)((rgba >> 24) & 0xFFu);
}

static inline void rn_mesh_block_init(ps2_vk_mesh_block *b) {
    memset(b, 0, sizeof *b);
    b->zscale = 1.0f;
    ps2_gs_native_frame(&b->xoff, &b->yoff, &b->zmax);
}

#endif
