#ifndef RN_VP_INT_H
#define RN_VP_INT_H

#include "rn_int.h"

#include <math.h>
#include <string.h>

#define VP_QW_MASK 0x3FFu

typedef struct {
    ps2_vf vf[32];
    u16 vi[16];
    u8 *mem;
    u32 top;
    u32 tpc;
    u32 clip;
    u32 r;
    float q, p;
    int native;
} vpx;

typedef int (*vp_fn)(vpx *x, u32 pc, u32 *kick_at);

enum { VP_MX = 8, VP_MY = 4, VP_MZ = 2, VP_MW = 1, VP_MXYZ = 14, VP_MALL = 15 };

static inline float *vp_F(vpx *x, int r) { return x->vf[r].f; }

static inline const u8 *vp_q(vpx *x, u32 a) { return x->mem + ((a & VP_QW_MASK) * 16u); }

static inline void vp_lq(vpx *x, int r, u32 a, u32 mask) {
    const u8 *p = vp_q(x, a);
    for (int i = 0; i < 4; i++)
        if ((mask >> (3 - i)) & 1) memcpy(&x->vf[r].f[i], p + i * 4, 4);
}
static inline void vp_rd(vpx *x, u32 a, float *out) { memcpy(out, vp_q(x, a), 16); }
static inline float vp_rd_lane(vpx *x, u32 a, int lane) {
    float f;
    memcpy(&f, vp_q(x, a) + (u32)lane * 4u, 4);
    return f;
}
static inline void vp_sq(vpx *x, u32 a, const float *v, u32 mask) {
    u8 *p = x->mem + ((a & VP_QW_MASK) * 16u);
    for (int i = 0; i < 4; i++)
        if ((mask >> (3 - i)) & 1) memcpy(p + i * 4, &v[i], 4);
}
static inline u16 vp_ilw(vpx *x, u32 a, int lane) {
    u16 v;
    memcpy(&v, vp_q(x, a) + (u32)lane * 4u, 2);
    return v;
}
static inline void vp_isw(vpx *x, u32 a, int lane, u16 v16) {
    u32 v = v16;
    memcpy(x->mem + ((a & VP_QW_MASK) * 16u) + (u32)lane * 4u, &v, 4);
}

static inline void vp_rows(float *o, const float *r0, const float *r1,
                           const float *r2, const float *r3,
                           float a, float b, float c, float d, u32 mask) {
    for (int i = 0; i < 4; i++) {
        float acc;
        if (!((mask >> (3 - i)) & 1)) continue;
        acc = r0[i] * a;
        acc = acc + r1[i] * b;
        acc = acc + r2[i] * c;
        o[i] = acc + r3[i] * d;
    }
}

static inline s32 vp_ftoi(float f, int sh) { return ps2_cvt_w_s(f * (float)(1u << sh)); }
static inline float vp_itof(s32 v, int sh) { return (float)v / (float)(1u << sh); }
static inline float vp_lane_itof(float bits, int sh) {
    s32 v;
    memcpy(&v, &bits, 4);
    return vp_itof(v, sh);
}
static inline float vp_lane_ftoi(float f, int sh) {
    s32 v = vp_ftoi(f, sh);
    float r;
    memcpy(&r, &v, 4);
    return r;
}
static inline float vp_max(float a, float b) { return a > b ? a : b; }
static inline float vp_min(float a, float b) { return a < b ? a : b; }

void pkt_put(u32 idx, const u32 w[4]);
void pkt_putf(u32 idx, const float *f);
void pkt_put_xyzf(u32 idx, const float *pre, s32 wlane);
u32 pkt_len(void);
void pkt_flush(void);
void pkt_kick(u32 at);

int  rn_vp_model(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_aircraft(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_part(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_terrain(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_detail(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_shadow(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_trees(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_loadmap(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_lit(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_skin(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_fx1(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_fx2(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_rain(vpx *x, u32 pc, u32 *kick_at);
int  rn_vp_flat(vpx *x, u32 pc, u32 *kick_at);
void rn_vp_model_report(void);
void rn_vp_terrain_report(void);
void rn_vp_shadow_report(void);
void rn_vp_sprite_report(void);
void rn_vp_load_report(void);
void rn_vp_lit_report(void);
void rn_vp_fx_report(void);
void rn_vp_flat_report(void);

int vp_mesh_wanted(void);

static inline int vp_tag_prim(vpx *x, u32 tmpl, u32 *prim) {
    u64 tag;
    memcpy(&tag, vp_q(x, tmpl), 8);
    if (!((tag >> 46) & 1u)) return 0;
    *prim = (u32)((tag >> 47) & 0x7FFu);
    return 1;
}

static inline int vp_prim_tris(u32 prim) {
    u32 t = prim & 7u;
    return t == 3u || t == 4u || t == 5u;
}

static inline int vp_tri_of(u32 prim_type, u32 k, u32 idx[3]) {
    if (k < 2u) return 0;
    if (prim_type == 3u) {
        if (k % 3u != 2u) return 0;
        idx[0] = k - 2u; idx[1] = k - 1u; idx[2] = k;
    } else if (prim_type == 5u) {
        idx[0] = 0; idx[1] = k - 1u; idx[2] = k;
    } else {
        idx[0] = k - 2u; idx[1] = k - 1u; idx[2] = k;
    }
    return 1;
}

static inline u32 vp_tri_indices(u32 prim_type, u32 count, int cull, u16 parity_base,
                                 u16 flags, u32 *idx) {
    u32 ni = 0;
    for (u32 k = 2; k < count; k++) {
        u32 t[3];
        if (!vp_tri_of(prim_type, k, t)) continue;
        if (cull) {
            u16 par = (u16)(parity_base + k);
            int negate = (flags & 0x80u) ? !(par & 1u) : (par & 1u);
            if (negate) { u32 tmp = t[1]; t[1] = t[2]; t[2] = tmp; }
        }
        idx[ni++] = t[0]; idx[ni++] = t[1]; idx[ni++] = t[2];
    }
    return ni;
}

static inline u32 vp_tri_index_bound(u32 count) {
    return count >= 3u ? (count - 2u) * 3u : 0u;
}

static inline void vp_copy_records(vpx *x, u32 rec0, u32 stride, u32 count, u8 *recs) {
    for (u32 k = 0; k < count; k++) memcpy(recs + k * 64u, vp_q(x, rec0 + stride * k), 64);
}

static inline u16 vp_clip_code(const float *c) {
    u16 code = 0;
    if (signbit(c[0] + c[3])) code |= 0x400;
    if (signbit(c[1] + c[3])) code |= 0x200;
    if (signbit(c[2] + c[3])) code |= 0x100;
    if (signbit(c[3] - c[0])) code |= 0x080;
    if (signbit(c[3] - c[1])) code |= 0x040;
    return code;
}

#endif
