#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

#define F(r) (x->vf[r].f)

static u64 st_rib_batches, st_rib_tris, st_rib_inside, st_rib_clipped, st_rib_rejected,
           st_rib_degenerate;

static u16 mac4(const float *r, u32 dest) {
    u16 mac = 0;
    for (int i = 0; i < 4; i++) {
        u32 u;
        if (!((dest >> (3 - i)) & 1u)) continue;
        memcpy(&u, &r[i], 4);
        if ((u & 0x7FFFFFFFu) == 0u) mac |= (u16)(1u << (3 - i));
        else if (u & 0x80000000u) mac |= (u16)(1u << (7 - i));
    }
    return mac;
}
static int neg_nz(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) && (u & 0x7FFFFFFFu);
}
static float vmax(float a, float b) { return ps2_vu_select(a, b, 0); }
static float vmin(float a, float b) { return ps2_vu_select(a, b, 1); }
static float vabs(float a) { u32 u; memcpy(&u, &a, 4); u &= 0x7FFFFFFFu; memcpy(&a, &u, 4); return a; }

#define fa ps2_fadd
#define fs ps2_fsub
#define fm ps2_fmul

static void clipw(vpx *x, const float *c) {
    u32 cl = (x->clip << 6) & 0xFFFFFFu;
    float w = c[3], aw = w < 0.0f ? -w : w;
    for (int i = 0; i < 3; i++) {
        if (c[i] > aw) cl |= 1u << (i * 2);
        if (c[i] < -aw) cl |= 1u << (i * 2 + 1);
    }
    x->clip = cl & 0xFFFFFFu;
}

static void rows4(float *o, vpx *x, int r0, const float *p) {
    const float *a = F(r0), *b = F(r0 + 1), *c = F(r0 + 2), *d = F(r0 + 3);
    float q[4] = { p[0], p[1], p[2], p[3] };
    for (int i = 0; i < 4; i++) {
        float acc = fm(a[i], q[0]);
        acc = fa(acc, fm(b[i], q[1]));
        acc = fa(acc, fm(c[i], q[2]));
        o[i] = fa(acc, fm(d[i], q[3]));
    }
}

static u32 emit(vpx *x, u32 top, u32 o) {
    float t1[4], t2[4], t3[4], t4[4];
    u32 w[4];
    u16 vi7 = x->vi[7];
    vp_rd(x, 0x1Cu, t1);
    vp_rd(x, top + 0u, t2);
    vp_rd(x, top + 1u, t3);
    vp_rd(x, 0x1Du, t4);
    pkt_putf(o++, t1);
    pkt_putf(o++, t2);
    pkt_putf(o++, t3);
    memcpy(w, t4, 16);
    w[0] = (u16)(vp_ilw(x, 0x1Du, 0) | x->vi[12]);
    pkt_put(o++, w);
    memcpy(F(1), t1, 16); memcpy(F(2), t2, 16); memcpy(F(3), t3, 16); memcpy(F(4), t4, 16);
    x->vi[8] = (u16)(x->vi[8] + 4u);
    do {
        float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v5 = F(5), *v6 = F(6);
        float q, pre[4];
        u16 vi4 = x->vi[4];
        vp_lq(x, 1, vi4 + 2u, VP_MALL);
        vp_lq(x, 2, vi4 + 0u, VP_MALL);
        vp_lq(x, 3, vi4 + 1u, VP_MALL);
        vp_lq(x, 4, top + 5u, VP_MALL);
        v1[3] = 1.0f;
        vp_lq(x, 5, 0x1Au, VP_MALL);
        {   float p[4] = { v1[0], v1[1], v1[2], v1[3] };
            rows4(v1, x, 10, p); }
        v6[3] = fm(v1[3], v5[1]);
        q = ps2_fdiv(1.0f, v1[3]);
        v6[3] = fa(v6[3], v5[0]);
        for (int i = 0; i < 3; i++) v1[i] = fm(v1[i], q);
        v2[2] = fm(v2[2], q);
        v4[3] = fm(v6[3], v3[3]);
        memcpy(pre, v1, 16);
        for (int i = 0; i < 3; i++) { s32 k = vp_ftoi(v1[i], 4); memcpy(&v1[i], &k, 4); }
        v2[0] = fm(v2[0], v2[2]); v2[1] = fm(v2[1], v2[2]);
        v1[3] = 0.0f;
        {   s32 k = vp_ftoi(v4[3], 0); memcpy(&v4[3], &k, 4); }
        pkt_putf(o++, v2);
        pkt_putf(o++, v4);
        pkt_put_xyzf(o++, pre, 0);
        vp_sq(x, x->vi[8], v2, VP_MALL);
        vp_sq(x, x->vi[8] + 1u, v4, VP_MALL);
        vp_sq(x, x->vi[8] + 2u, v1, VP_MALL);
        x->vi[4] = (u16)(vi4 + 4u);
        x->vi[12]--;
        x->vi[8] = (u16)(x->vi[8] + 3u);
    } while ((s16)x->vi[12] > 0);
    {   float term[4];
        vp_rd(x, 0x1Fu, term);
        memcpy(F(1), term, 16);
        pkt_putf(o++, term);
        vp_sq(x, x->vi[8], term, VP_MALL);
    }
    x->vi[1] = 0x320;
    pkt_kick(vi7);
    x->vi[7] = vi7 == 0x320u ? 0x384 : 0x320;
    x->vi[8] = x->vi[7];
    return o;
}

static u32 triangle(vpx *x, u32 top, u32 o) {
    float *v18 = F(18), *v19 = F(19), *v20 = F(20);
    x->vi[12] = 3;
    x->vi[11] = 1;
    x->vi[4] = (u16)(top + 0x28u);
    x->vi[3] = (u16)(top + 0x50u);
    vp_lq(x, 1, top + 0x2Au, VP_MALL);
    vp_lq(x, 2, top + 0x2Eu, VP_MALL);
    vp_lq(x, 3, top + 0x32u, VP_MALL);
    rows4(v18, x, 14, F(1));
    rows4(v19, x, 14, F(2));
    rows4(v20, x, 14, F(3));
    clipw(x, v18);
    clipw(x, v19);
    clipw(x, v20);
    st_rib_tris++;
    {   static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("PS2_RN_FXDBG"); dbg = e ? atoi(e) : 0; }
        if (dbg > 0) {
            dbg--;
            ps2_log("fx2 tri: clip %06X  c0 %g %g %g %g  c1 %g %g %g %g  c2 %g %g %g %g",
                    x->clip, v18[0], v18[1], v18[2], v18[3], v19[0], v19[1], v19[2], v19[3],
                    v20[0], v20[1], v20[2], v20[3]);
        }
    }
    if (!(x->clip & 0x03FFFFu)) {
        x->vi[1] = 0;
        st_rib_inside++;
        return emit(x, top, o);
    }
    {   static const u32 planes[6] = { 0xFFEFBEu, 0xFFDF7Du, 0xFFBEFBu, 0xFF7DF7u,
                                       0xFEFBEFu, 0xFDF7DFu };
        for (int k = 0; k < 6; k++)
            if (((x->clip | planes[k]) & 0xFFFFFFu) == 0xFFFFFFu) {
                x->vi[1] = 1;
                st_rib_rejected++;
                return o;
            }
        x->vi[1] = 0;
    }
    st_rib_clipped++;
    F(4)[0] = 0.0f + 1.0f;
    vp_sq(x, x->vi[4] + 3u, v18, VP_MALL);
    vp_sq(x, x->vi[4] + 7u, v19, VP_MALL);
    vp_sq(x, x->vi[4] + 11u, v20, VP_MALL);
    for (;;) {
        u16 vi13 = x->vi[12];
        u16 vi1 = (u16)((u16)(vi13 - 1u) * 4u + x->vi[4]);
        for (int k = 0; k < 4; k++) vp_lq(x, 18 + k, vi1 + (u32)k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 22 + k, x->vi[4] + (u32)k, VP_MALL);
        x->vi[4] = (u16)(x->vi[4] + 4u);
        do {
            float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4);
            float *v21 = F(21), *v25 = F(25);
            u16 vi2;
            if (x->vi[11] & 3u)        { v4[1] = fa(0.0f, v21[0]); v4[2] = fa(0.0f, v25[0]); }
            else if (x->vi[11] & 0xCu) { v4[1] = fa(0.0f, v21[1]); v4[2] = fa(0.0f, v25[1]); }
            else                       { v4[1] = fa(0.0f, v21[2]); v4[2] = fa(0.0f, v25[2]); }
            v3[0] = fm(v4[0], v4[1]);
            v2[0] = fm(v4[0], v4[2]);
            vi2 = 0;
            if (!neg_nz(fs(v3[0], v21[3]))) vi2 = (u16)(vi2 + 1u);
            if (!neg_nz(fs(v2[0], v25[3]))) vi2 = (u16)(vi2 + 2u);
            v1[0] = fm(v4[0], v21[3]);
            v3[0] = fm(v4[0], v25[3]);
            v2[1] = fs(v4[1], v1[0]);
            v2[2] = fs(v4[2], v3[0]);
            if (vi2 == 0u) {
                for (int k = 0; k < 4; k++) vp_sq(x, x->vi[3] + (u32)k, F(18 + k), VP_MALL);
                x->vi[3] = (u16)(x->vi[3] + 4u);
            } else {
                v3[2] = fs(v2[2], v2[1]);
                if (vi2 == 2u) {
                    float q = ps2_fdiv(1.0f, v3[2]);
                    for (int k = 0; k < 4; k++) vp_sq(x, x->vi[3] + (u32)k, F(18 + k), VP_MALL);
                    x->vi[3] = (u16)(x->vi[3] + 4u);
                    v2[1] = fm(v2[1], q);
                    for (int i = 0; i < 4; i++) {
                        F(5)[i] = fs(F(22)[i], F(18)[i]);
                        F(6)[i] = fs(F(23)[i], F(19)[i]);
                        F(7)[i] = fs(F(24)[i], F(20)[i]);
                    }
                    v2[1] = vabs(v2[1]);
                    x->vi[12]++;
                    for (int i = 0; i < 4; i++) F(7)[i] = fm(F(7)[i], v2[1]);
                    for (int i = 0; i < 4; i++) F(5)[i] = fm(F(5)[i], v2[1]);
                    for (int i = 0; i < 4; i++) F(6)[i] = fm(F(6)[i], v2[1]);
                    for (int i = 0; i < 4; i++) F(7)[i] = fa(F(7)[i], F(20)[i]);
                    for (int i = 0; i < 4; i++) F(5)[i] = fa(F(5)[i], F(18)[i]);
                    for (int i = 0; i < 4; i++) F(6)[i] = fa(F(6)[i], F(19)[i]);
                    rows4(F(8), x, 14, F(7));
                    for (int k = 0; k < 4; k++) vp_sq(x, x->vi[3] + (u32)k, F(5 + k), VP_MALL);
                    x->vi[3] = (u16)(x->vi[3] + 4u);
                } else {
                    v3[1] = fs(v2[1], v2[2]);
                    if (vi2 == 1u) {
                        float q = ps2_fdiv(1.0f, v3[1]);
                        v2[2] = fm(v2[2], q);
                        for (int i = 0; i < 4; i++) {
                            F(5)[i] = fs(F(18)[i], F(22)[i]);
                            F(6)[i] = fs(F(19)[i], F(23)[i]);
                            F(7)[i] = fs(F(20)[i], F(24)[i]);
                        }
                        v2[2] = vabs(v2[2]);
                        for (int i = 0; i < 4; i++) F(7)[i] = fm(F(7)[i], v2[2]);
                        for (int i = 0; i < 4; i++) F(5)[i] = fm(F(5)[i], v2[2]);
                        for (int i = 0; i < 4; i++) F(6)[i] = fm(F(6)[i], v2[2]);
                        for (int i = 0; i < 4; i++) F(7)[i] = fa(F(7)[i], F(24)[i]);
                        for (int i = 0; i < 4; i++) F(5)[i] = fa(F(5)[i], F(22)[i]);
                        for (int i = 0; i < 4; i++) F(6)[i] = fa(F(6)[i], F(23)[i]);
                        rows4(F(8), x, 14, F(7));
                        for (int k = 0; k < 4; k++) vp_sq(x, x->vi[3] + (u32)k, F(5 + k), VP_MALL);
                        x->vi[3] = (u16)(x->vi[3] + 4u);
                    } else {
                        x->vi[12]--;
                    }
                }
            }
            for (int k = 0; k < 4; k++) memcpy(F(18 + k), F(22 + k), 16);
            for (int k = 0; k < 4; k++) vp_lq(x, 22 + k, x->vi[4] + (u32)k, VP_MALL);
            x->vi[4] = (u16)(x->vi[4] + 4u);
            vi13--;
        } while ((s16)vi13 > 0);
        x->vi[13] = vi13;
        if ((s16)(u16)(x->vi[4] - (u16)(top + 0x50u)) > 0) {
            x->vi[4] = (u16)(top + 0x28u);
            x->vi[3] = (u16)(top + 0x50u);
        } else {
            x->vi[4] = (u16)(top + 0x50u);
            x->vi[3] = (u16)(top + 0x28u);
        }
        F(4)[0] = fm(F(4)[0], -1.0f);
        if ((s16)(u16)(x->vi[12] - 2u) <= 0) { x->vi[1] = (u16)(x->vi[12] - 2u); st_rib_degenerate++; return o; }
        x->vi[2] = 0x40;
        x->vi[11] = (u16)(x->vi[11] + x->vi[11]);
        if (x->vi[11] == 0x40u) break;
    }
    return emit(x, top, o);
}

int rn_vp_fx2(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, o = 0;
    (void)kick_at;
    if (pc == 0x0000u) {
        for (int k = 0; k < 8; k++) vp_lq(x, 10 + k, 0x10u + (u32)k, VP_MALL);
        x->tpc = 0x0050u;
        return 0;
    }
    if (pc != 0x0050u) return -1;
    top = x->top & VP_QW_MASK;
    st_rib_batches++;
    x->vi[9] = (u16)top;
    x->vi[7] = vp_ilw(x, 0x18u, 0);
    x->vi[5] = 0;
    x->vi[8] = x->vi[7];
    F(30)[0] = -1.0f;
    for (;;) {
        float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v5 = F(5), *v6 = F(6);
        float *v7 = F(7), *v20 = F(20), *v30 = F(30);
        u16 seg = x->vi[5], mac;
        float q;
        v30[0] = fa(v30[0], 1.0f);
        vp_lq(x, 1, top + 2u, VP_MALL);
        v20[0] = fm(v30[0], 0.125f);
        v20[0] = fa(v20[0], v1[0]);
        v20[0] = vmax(v20[0], 0.0f);
        v20[0] = vmin(v20[0], 1.0f);
        if (seg == 0u) {
            v5[3] = 1.0f * 0.0f;
        } else if (!neg_nz(fs(v20[0], 0.7f))) {
            v5[3] = 1.0f * 0.0f;
        } else if (!neg_nz(fs(v20[0], 0.5f))) {
            v5[0] = fm(v20[0], -5.0f);
            v5[0] = fa(v5[0], 3.5f);
            v5[0] = fm(v5[0], v1[2]);
            v5[0] = fs(v5[0], v1[3]);
            v5[3] = fm(1.0f, v5[0]);
        } else {
            v5[2] = fs(v1[2], v1[3]);
            v5[3] = fm(1.0f, v5[2]);
        }
        v5[3] = vmax(v5[3], 0.0f);
        v5[3] = vmin(v5[3], 127.0f);
        vp_sq(x, top + 0x22u, v5, VP_MALL);
        vp_sq(x, top + 0x25u, v5, VP_MALL);
        v20[0] = fm(v20[0], 255.0f);
        vp_lq(x, 1, top + 0x17u, VP_MALL);
        vp_lq(x, 2, top + 0x18u, VP_MALL);
        vp_lq(x, 3, top + 0x19u, VP_MALL);
        vp_lq(x, 4, top + 0x1Au, VP_MALL);
        for (int i = 0; i < 4; i++) F(18)[i] = vmin(v1[i], v20[0]);
        for (int i = 0; i < 4; i++) F(19)[i] = vmin(v2[i], v20[0]);
        for (int i = 0; i < 4; i++) v7[i] = 0.0f * 0.0f;
        v7[3] = 1.0f * 0.0f;
        {   float r[4];
            for (int i = 0; i < 4; i++) r[i] = fs(F(18)[i], v20[0]);
            mac = mac4(r, 0xF);
        }
        v5[0] = 0.0f; v5[1] = 1.0f;
        v5[2] = fa(v7[2], v3[0]); v5[3] = fa(v7[3], v3[0]);
        if (!(mac & 8u)) {
            v5[0] = fa(v7[0], v1[0]); v5[1] = fa(v7[1], v1[1]); v5[2] = fa(v7[2], v3[0]); v5[3] = fa(v7[3], v3[1]);
            if (!(mac & 4u)) {
                v5[0] = fa(v7[0], v1[1]); v5[1] = fa(v7[1], v1[2]); v5[2] = fa(v7[2], v3[1]); v5[3] = fa(v7[3], v3[2]);
                if (!(mac & 2u)) {
                    v5[0] = fa(v7[0], v1[2]); v5[1] = fa(v7[1], v1[3]); v5[2] = fa(v7[2], v3[2]); v5[3] = fa(v7[3], v3[3]);
                    if (!(mac & 1u)) {
                        float r[4];
                        for (int i = 0; i < 4; i++) r[i] = fs(F(19)[i], v20[0]);
                        mac = mac4(r, 0xF);
                        v5[0] = fa(v7[0], v1[3]); v5[1] = fa(v7[1], v2[0]); v5[2] = fa(v7[2], v3[3]); v5[3] = fa(v7[3], v4[0]);
                        if (!(mac & 8u)) {
                            v5[0] = fa(v7[0], v2[0]); v5[1] = fa(v7[1], v2[1]); v5[2] = fa(v7[2], v4[0]); v5[3] = fa(v7[3], v4[1]);
                            if (!(mac & 4u)) {
                                v5[0] = fa(v7[0], v2[1]); v5[1] = fa(v7[1], v2[2]); v5[2] = fa(v7[2], v4[1]); v5[3] = fa(v7[3], v4[2]);
                                if (!(mac & 2u)) {
                                    v5[0] = fa(v7[0], v2[2]); v5[1] = fa(v7[1], v2[3]);
                                    v5[2] = fa(v7[2], v4[2]); v5[3] = fa(v7[3], v4[3]);
                                }
                            }
                        }
                    }
                }
            }
        }
        v1[1] = fs(v5[1], v5[0]);
        v1[3] = fs(v5[3], v5[2]);
        {
            const u32 ib = 0x3B808080u;
            float i255;
            memcpy(&i255, &ib, 4);
            v20[0] = fm(v20[0], i255);
            v1[0] = fm(v5[0], i255);
            q = ps2_fdiv(v1[3], v1[1]);
            v1[2] = fm(v5[2], i255);
        }
        v1[0] = fs(v20[0], v1[0]);
        v1[0] = fm(v1[0], q);
        v1[0] = fa(v1[0], v1[2]);
        vp_lq(x, 2, top + 2u, VP_MALL);
        v1[0] = fm(v1[0], v2[1]);
        vp_lq(x, 2, top + 0x0Fu + seg, VP_MALL);
        vp_lq(x, 3, top + 0x07u + seg, VP_MALL);
        v2[0] = fm(v2[0], v1[0]); v2[2] = fm(v2[2], v1[0]);
        for (int i = 0; i < 3; i++) v4[i] = fa(v2[i], v3[i]);
        v4[3] = 1.0f + 0.0f;
        v2[0] = fm(v2[0], -1.0f); v2[2] = fm(v2[2], -1.0f);
        vp_sq(x, top + 0x23u, v4, VP_MALL);
        for (int i = 0; i < 3; i++) v6[i] = fa(v2[i], v3[i]);
        v6[3] = 1.0f + 0.0f;
        vp_sq(x, top + 0x26u, v6, VP_MALL);
        x->vi[1] = (u16)(top + 0x0Fu + seg);
        x->vi[2] = (u16)(top + 0x07u + seg);
        if (seg != 0u) {
            float r[4];
            vp_lq(x, 1, top + 6u, VP_MALL);
            v2[0] = fa(0.0f, v1[0]); v2[1] = fa(0.0f, v1[1]); v2[2] = 0.0f + 1.0f;
            v3[0] = fa(0.0f, v1[2]); v3[1] = fa(0.0f, v1[1]); v3[2] = 0.0f + 1.0f;
            v4[0] = fa(0.0f, v1[0]); v4[1] = fa(0.0f, v1[3]); v4[2] = 0.0f + 1.0f;
            v5[0] = fa(0.0f, v1[2]); v5[1] = fa(0.0f, v1[3]); v5[2] = 0.0f + 1.0f;
            vp_sq(x, top + 0x1Bu, v2, VP_MALL);
            vp_sq(x, top + 0x1Eu, v3, VP_MALL);
            vp_sq(x, top + 0x21u, v4, VP_MALL);
            vp_sq(x, top + 0x24u, v5, VP_MALL);
            (void)r;
            x->vi[10] = 0;
            while (x->vi[10] != 2u) {
                u32 src = top + (x->vi[10] ? 3u : 0u);
                static const u8 from[9] = { 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23 };
                static const u8 to[9]   = { 0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E, 0x30, 0x31, 0x32 };
                for (int k = 0; k < 9; k++) {
                    float t[4];
                    vp_rd(x, src + from[k], t);
                    vp_sq(x, top + to[k], t, VP_MALL);
                    if (k < 4) memcpy(F(1 + k), t, 16);
                    else if (k < 8) memcpy(F(1 + k - 4), t, 16);
                    else memcpy(F(1), t, 16);
                }
                x->vi[10]++;
                x->vi[1] = (u16)src;
                o = triangle(x, top, o);
            }
        }
        vp_isw(x, 0x18u, 0, x->vi[7]);
        {   float t[4];
            vp_rd(x, top + 0x22u, t); memcpy(F(1), t, 16); vp_sq(x, top + 0x1Cu, t, VP_MALL);
            vp_rd(x, top + 0x23u, t); memcpy(F(2), t, 16); vp_sq(x, top + 0x1Du, t, VP_MALL);
            vp_rd(x, top + 0x25u, t); memcpy(F(3), t, 16); vp_sq(x, top + 0x1Fu, t, VP_MALL);
            vp_rd(x, top + 0x26u, t); memcpy(F(4), t, 16); vp_sq(x, top + 0x20u, t, VP_MALL);
        }
        x->vi[5]++;
        if (x->vi[5] == 8u) break;
    }
    x->vi[1] = 0x1F;
    {   float term[4];
        vp_rd(x, 0x1Fu, term);
        pkt_putf(o++, term);
        pkt_kick(0x1Fu);
    }
    x->tpc = 0x0050u;
    return 1;
}

static u64 st_pt_batches, st_pt_particles, st_pt_packets, st_pt_clipped, st_pt_rejected,
           st_pt_faded, st_pt_lines, st_pt_sprites, st_pt_declined, st_pt_cut[2], st_pt_steps[5];

static void rinit(vpx *x, float f) { u32 u; memcpy(&u, &f, 4); x->r = u & 0x7FFFFFu; }
static float rnext(vpx *x) {
    u32 r = x->r, b;
    float f;
    r = ((r << 1) | (((r >> 22) ^ (r >> 21)) & 1u)) & 0x7FFFFFu;
    x->r = r;
    b = 0x3F800000u | r;
    memcpy(&f, &b, 4);
    return f;
}
static u16 mtir(float f) { u32 u; memcpy(&u, &f, 4); return (u16)u; }
static float imm(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static float ftz(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    if (!(u & 0x7F800000u)) { u &= 0x80000000u; memcpy(&f, &u, 4); }
    return f;
}
static float eleng(const float *v) {
    return sqrtf(ftz(ftz(ftz(v[0] * v[0]) + ftz(v[1] * v[1])) + ftz(v[2] * v[2])));
}
static void mr32(float *v) { float t = v[0]; v[0] = v[1]; v[1] = v[2]; v[2] = v[3]; v[3] = t; }

static void fx1_curve(vpx *x, float *o) {
    const float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *z = F(15);
    float t = F(20)[0], r[4];
    u16 mac;
    for (int i = 0; i < 4; i++) F(18)[i] = vmin(v1[i], t);
    for (int i = 0; i < 4; i++) F(19)[i] = vmin(v2[i], t);
    for (int i = 0; i < 4; i++) r[i] = fs(F(18)[i], t);
    mac = mac4(r, 0xF);
    x->vi[2] = mac;
#define SEG(a, b, c, d) do { o[0] = fa(z[0], a); o[1] = fa(z[1], b); \
                             o[2] = fa(z[2], c); o[3] = fa(z[3], d); } while (0)
    o[0] = 0.0f; o[1] = 1.0f; o[2] = fa(z[2], v3[0]); o[3] = fa(z[3], v3[0]);
    if ((x->vi[1] = mac & 8u)) return;
    SEG(v1[0], v1[1], v3[0], v3[1]);
    if ((x->vi[1] = mac & 4u)) return;
    SEG(v1[1], v1[2], v3[1], v3[2]);
    if ((x->vi[1] = mac & 2u)) return;
    SEG(v1[2], v1[3], v3[2], v3[3]);
    if ((x->vi[1] = mac & 1u)) return;
    for (int i = 0; i < 4; i++) r[i] = fs(F(19)[i], t);
    mac = mac4(r, 0xF);
    x->vi[2] = mac;
    SEG(v1[3], v2[0], v3[3], v4[0]);
    if ((x->vi[1] = mac & 8u)) return;
    SEG(v2[0], v2[1], v4[0], v4[1]);
    if ((x->vi[1] = mac & 4u)) return;
    SEG(v2[1], v2[2], v4[1], v4[2]);
    if ((x->vi[1] = mac & 2u)) return;
    SEG(v2[2], v2[3], v4[2], v4[3]);
    x->vi[1] = mac & 1u;
#undef SEG
}

static int fx1_clip_line(vpx *x, u32 top) {
    x->vi[11] = 1;
    x->vi[4] = (u16)(top - 8u);
    F(4)[0] = 1.0f;
    vp_sq(x, x->vi[4] + 0u, F(22), VP_MALL);
    vp_sq(x, x->vi[4] + 1u, F(18), VP_MALL);
    vp_sq(x, x->vi[4] + 2u, F(23), VP_MALL);
    vp_sq(x, x->vi[4] + 3u, F(19), VP_MALL);
    x->vi[3] = (u16)(top - 4u);
    do {
        float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v18 = F(18), *v19 = F(19);
        float q;
        int n0, n1;
        vp_lq(x, 18, x->vi[4] + 1u, VP_MALL);
        vp_lq(x, 19, x->vi[4] + 3u, VP_MALL);
        v4[1] = fa(0.0f, v18[0]);
        v4[2] = fa(0.0f, v19[0]);
        x->vi[2] = (u16)(3u & x->vi[11]);
        if (!x->vi[2]) {
            v4[1] = fa(0.0f, v18[1]);
            v4[2] = fa(0.0f, v19[1]);
            x->vi[2] = (u16)(0xCu & x->vi[11]);
            if (!x->vi[2]) { v4[1] = fa(0.0f, v18[2]); v4[2] = fa(0.0f, v19[2]); }
        }
        v3[0] = fm(v4[0], v4[1]);
        v2[0] = fm(v4[0], v4[2]);
        n0 = neg_nz(fs(v3[0], v18[3]));
        n1 = neg_nz(fs(v2[0], v19[3]));
        x->vi[2] = (u16)((n0 ? 0u : 1u) + (n1 ? 0u : 2u));
        x->vi[1] = n1 ? 0x80u : 0u;
        v1[0] = fm(v4[0], v18[3]);
        v3[0] = fm(v4[0], v19[3]);
        v2[1] = fs(v4[1], v1[0]);
        v2[2] = fs(v4[2], v3[0]);
        if (x->vi[2] == 0u) {
            vp_lq(x, 22, x->vi[4] + 0u, VP_MALL);
            vp_lq(x, 23, x->vi[4] + 2u, VP_MALL);
            vp_sq(x, x->vi[3] + 1u, F(18), VP_MALL);
            vp_sq(x, x->vi[3] + 3u, F(19), VP_MALL);
            vp_sq(x, x->vi[3] + 0u, F(22), VP_MALL);
            vp_sq(x, x->vi[3] + 2u, F(23), VP_MALL);
        } else {
            x->vi[1] = 2;
            v3[2] = fs(v2[2], v2[1]);
            if (x->vi[2] == 2u) {
                st_pt_cut[1]++;
                vp_lq(x, 22, x->vi[4] + 0u, VP_MALL);
                q = ps2_fdiv(1.0f, v3[2]);
                vp_lq(x, 23, x->vi[4] + 2u, VP_MALL);
                vp_sq(x, x->vi[3] + 1u, F(18), VP_MALL);
                vp_sq(x, x->vi[3] + 0u, F(22), VP_MALL);
                v2[1] = fm(v2[1], q);
                for (int i = 0; i < 4; i++) v1[i] = fs(F(23)[i], F(22)[i]);
                v2[1] = vabs(v2[1]);
                for (int i = 0; i < 4; i++) v1[i] = fm(v1[i], v2[1]);
                for (int i = 0; i < 4; i++) F(23)[i] = fa(v1[i], F(22)[i]);
                v2[0] = fa(0.0f, F(23)[3]);
                F(23)[3] = 1.0f;
                rows4(F(19), x, 9, F(23));
                F(23)[3] = fm(1.0f, v2[0]);
                vp_sq(x, x->vi[3] + 2u, F(23), VP_MALL);
                vp_sq(x, x->vi[3] + 3u, F(19), VP_MALL);
                x->q = q;
            } else {
                x->vi[1] = 1;
                v3[1] = fs(v2[1], v2[2]);
                if (x->vi[2] != 1u) return 0;
                st_pt_cut[0]++;
                vp_lq(x, 22, x->vi[4] + 0u, VP_MALL);
                q = ps2_fdiv(1.0f, v3[1]);
                vp_lq(x, 23, x->vi[4] + 2u, VP_MALL);
                vp_sq(x, x->vi[3] + 3u, F(19), VP_MALL);
                vp_sq(x, x->vi[3] + 2u, F(23), VP_MALL);
                v2[2] = fm(v2[2], q);
                for (int i = 0; i < 4; i++) v1[i] = fs(F(22)[i], F(23)[i]);
                v2[2] = vabs(v2[2]);
                for (int i = 0; i < 4; i++) v1[i] = fm(v1[i], v2[2]);
                for (int i = 0; i < 4; i++) F(22)[i] = fa(v1[i], F(23)[i]);
                v2[0] = fa(0.0f, F(22)[3]);
                F(22)[3] = 1.0f;
                rows4(F(18), x, 9, F(22));
                F(22)[3] = fm(1.0f, v2[0]);
                vp_sq(x, x->vi[3] + 0u, F(22), VP_MALL);
                vp_sq(x, x->vi[3] + 1u, F(18), VP_MALL);
                x->q = q;
            }
        }
        x->vi[1] = (u16)(top - 8u);
        if (x->vi[4] != x->vi[1]) { x->vi[4] = (u16)(top - 8u); x->vi[3] = (u16)(top - 4u); }
        else                      { x->vi[4] = (u16)(top - 4u); x->vi[3] = (u16)(top - 8u); }
        v4[0] = fm(v4[0], -1.0f);
        x->vi[2] = 0x40;
        x->vi[11] = (u16)(x->vi[11] + x->vi[11]);
    } while (x->vi[11] != 0x40u);
    vp_lq(x, 22, x->vi[4] + 0u, VP_MALL);
    vp_lq(x, 23, x->vi[4] + 2u, VP_MALL);
    return 1;
}

static void fx1_sprites(vpx *x, u32 top) {
    int more;
    do {
        float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v15 = F(15), *v17 = F(17);
        float *v27 = F(27), *v28 = F(28), *v29 = F(29);
        v1[1] = fa(v1[1], 1.0f);
        x->vi[4] = (u16)(x->vi[4] + 1u);
        v2[1] = fm(v1[1], v27[3]);
        if (neg_nz(fs(v2[1], v27[1]))) { x->vi[1] = 0x40; goto next; }
        if (neg_nz(fs(v27[0], v2[1]))) { x->vi[1] = 0x80; goto next; }
        for (int i = 0; i < 4; i++) v3[i] = fm(F(14)[i], v2[1]);
        for (int i = 0; i < 3; i++) v4[i] = fm(F(13)[i], v2[1]);
        v3[1] = fa(v3[1], v28[0]);
        v3[3] = fa(v3[3], v28[2]);
        v3[0] = fa(v3[0], v17[1]);
        x->vi[1] = (u16)(1u & x->vi[4]);
        for (int i = 0; i < 3; i++) v4[i] = fa(v4[i], F(23)[i]);
        if ((s16)x->vi[1] > 0) v3[1] = fm(v3[1], v17[2]);
        rinit(x, v3[0]);
        v3[2] = rnext(x);
        vp_lq(x, 15, 0x1Bu, VP_MALL);
        {   int thin = neg_nz(fs(v3[1], 1.0f));
            v3[2] = fs(v3[2], 1.0f);
            x->vi[1] = thin ? 0x40u : 0u;
            v4[3] = fm(v4[3], 0.0f);
            v3[2] = fm(v3[2], 127.0f);
            v2[0] = fm(v15[0], v3[3]);
            if (thin) goto next;
        }
        st_pt_sprites++;
        {   float *v18 = F(18), *v19 = F(19), *v20 = F(20), *v21 = F(21);
            u16 at = x->vi[8];
            v2[1] = fm(v15[1], v3[3]);
            memcpy(v18, v4, 16); memcpy(v19, v4, 16); memcpy(v20, v4, 16); memcpy(v21, v4, 16);
            v3[2] = vp_lane_ftoi(v3[2], 0);
            v18[3] = v19[3] = v20[3] = v21[3] = 1.0f;
            x->vi[1] = (u16)(mtir(v3[2]) + 0x21u);
            v18[0] = fs(v18[0], v2[0]);
            v19[1] = fs(v19[1], v2[1]);
            v20[1] = fa(v20[1], v2[1]);
            v21[0] = fa(v21[0], v2[0]);
            vp_lq(x, 29, x->vi[1], VP_MALL);
            for (int i = 0; i < 3; i++) {
                v18[i] = vp_lane_ftoi(v18[i], 4); v19[i] = vp_lane_ftoi(v19[i], 4);
                v20[i] = vp_lane_ftoi(v20[i], 4); v21[i] = vp_lane_ftoi(v21[i], 4);
            }
            v29[0] = fm(v29[0], 7.0f);
            vp_lq(x, 15, 0x1Du, VP_MALL);
            vp_sq(x, at + 3u, v18, VP_MALL);
            vp_sq(x, at + 6u, v19, VP_MALL);
            vp_sq(x, at + 9u, v20, VP_MALL);
            vp_sq(x, at + 12u, v21, VP_MALL);
            v29[0] = vp_lane_ftoi(v29[0], 0);
            vp_sq(x, at + 0u, v15, VP_MALL);
            vp_lq(x, 2, top + 0x0Bu, VP_MALL);
            vp_lq(x, 18, top + 7u, VP_MALL);
            v29[0] = vp_lane_itof(v29[0], 0);
            vp_lq(x, 19, top + 8u, VP_MALL);
            vp_lq(x, 20, top + 9u, VP_MALL);
            v29[1] = fm(v29[1], v2[3]);
            vp_lq(x, 21, top + 0x0Au, VP_MALL);
            v18[3] = fm(1.0f, v3[1]); v19[3] = fm(1.0f, v3[1]);
            v20[3] = fm(1.0f, v3[1]); v21[3] = fm(1.0f, v3[1]);
            v18[3] = vp_lane_ftoi(v18[3], 0); v19[3] = vp_lane_ftoi(v19[3], 0);
            v20[3] = vp_lane_ftoi(v20[3], 0); v21[3] = vp_lane_ftoi(v21[3], 0);
            v29[0] = fm(v29[0], 4.0f);
            vp_sq(x, at + 2u, v18, VP_MALL);
            vp_sq(x, at + 5u, v19, VP_MALL);
            vp_sq(x, at + 8u, v20, VP_MALL);
            vp_sq(x, at + 11u, v21, VP_MALL);
            v29[1] = vp_lane_ftoi(v29[1], 0);
            v29[0] = vp_lane_ftoi(v29[0], 0);
            v29[1] = vp_lane_itof(v29[1], 0);
            x->vi[1] = mtir(v29[0]);
            v29[1] = fm(v29[1], v2[2]);
            x->vi[1] = (u16)(x->vi[1] + 0xA2u);
            v2[0] = fa(v2[0], v29[1]);
            for (int k = 0; k < 4; k++) vp_lq(x, 18 + k, x->vi[1] + (u32)k, VP_MALL);
            for (int k = 0; k < 4; k++) {
                float *v = F(18 + k);
                v[0] = fm(v[0], v2[2]); v[1] = fm(v[1], v2[2]);
            }
            for (int k = 0; k < 4; k++) {
                float *v = F(18 + k);
                v[0] = fa(v[0], v2[0]); v[1] = fa(v[1], v2[1]);
            }
            for (int k = 0; k < 4; k++) {
                float *v = F(18 + k);
                v[0] = vp_lane_ftoi(v[0], 0); v[1] = vp_lane_ftoi(v[1], 0);
            }
            vp_sq(x, at + 1u, v18, VP_MALL);
            vp_sq(x, at + 4u, v19, VP_MALL);
            vp_sq(x, at + 7u, v20, VP_MALL);
            vp_sq(x, at + 10u, v21, VP_MALL);
            x->vi[8] = (u16)(at + 0x0Du);
        }
    next:
        more = neg_nz(fs(F(1)[1], F(1)[0]));
        x->vi[1] = more ? 0x40u : 0u;
    } while (more);
}

static u32 fx1_particle(vpx *x, u32 top, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v13 = F(13), *v14 = F(14);
    float *v15 = F(15), *v16 = F(16), *v17 = F(17), *v20 = F(20), *v22 = F(22), *v23 = F(23);
    float *v27 = F(27), *v28 = F(28), *v29 = F(29);
    float q;
    st_pt_particles++;
    F(26)[0] = fa(F(26)[0], 1.0f);
    vp_lq(x, 1, top + 4u, VP_MALL);
    vp_lq(x, 18, x->vi[10] + 0u, VP_MALL);
    vp_lq(x, 19, x->vi[10] + 1u, VP_MALL);
    x->vi[10] = (u16)(x->vi[10] + 2u);
    v2[0] = fm(F(26)[0], v1[1]);
    v17[3] = fm(F(19)[3], 53.0f);
    vp_lq(x, 3, top + 3u, VP_MALL);
    v2[0] = fa(v1[0], v2[0]);
    v17[3] = fa(v17[3], v3[3]);
    v17[0] = fa(0.0f, v17[3]);
    v2[0] = fm(v2[0], v1[2]);
    rinit(x, v17[3]);
    v13[0] = rnext(x);
    v13[0] = fs(v13[0], 1.0f);
    v13[0] = fm(v13[0], 127.0f);
    v13[0] = vp_lane_ftoi(v13[0], 0);
    x->vi[1] = (u16)(mtir(v13[0]) + 0x21u);
    vp_lq(x, 29, x->vi[1], VP_MALL);
    {
        float acc = fa(0.0f, 1.0f);
        acc = fa(acc, fm(v29[0], imm(0x3E4CCCCDu)));
        v29[0] = fa(acc, fm(0.0f, 0.0f));
        acc = fa(0.0f, v1[3]);
        acc = fa(acc, fm(v2[0], v29[0]));
        v20[0] = fa(acc, fm(0.0f, 0.0f));
    }
    v20[0] = vmax(v20[0], 0.0f);
    v20[0] = vmin(v20[0], 1.0f);
    for (int i = 0; i < 3; i++) F(19)[i] = fm(F(19)[i], v20[0]);
    v20[0] = fm(v20[0], 255.0f);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, top + 0x10u + (u32)k, VP_MALL);
    for (int i = 0; i < 3; i++) F(24)[i] = fa(F(18)[i], F(19)[i]);
    F(24)[3] = 1.0f;
    for (int i = 0; i < 4; i++) v15[i] = fm(i == 3 ? 1.0f : 0.0f, 0.0f);
    fx1_curve(x, v13);
    for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, top + 0x0Cu + (u32)k, VP_MALL);
    fx1_curve(x, v14);
    {   const float i255 = imm(0x3B808080u);
        float q1, q2;
        v1[1] = fs(v13[1], v13[0]);
        v1[3] = fs(v13[3], v13[2]);
        v2[1] = fs(v14[1], v14[0]);
        v2[3] = fs(v14[3], v14[2]);
        v20[0] = fm(v20[0], i255);
        v1[0] = fm(v13[0], i255);
        q1 = ps2_fdiv(v1[3], v1[1]);
        v1[2] = fm(v13[2], i255);
        v2[0] = fm(v14[0], i255);
        v2[2] = fm(v14[2], i255);
        v1[0] = fs(v20[0], v1[0]);
        v2[0] = fs(v20[0], v2[0]);
        v1[0] = fm(v1[0], q1);
        q2 = ps2_fdiv(v2[3], v2[1]);
        v1[0] = fa(v1[0], v1[2]);
        v2[0] = fm(v2[0], q2);
        vp_lq(x, 13, top + 3u, VP_MALL);
        v2[0] = fa(v2[0], v2[2]);
        v16[1] = fa(0.0f, v1[0]);
        v16[3] = fm(1.0f, v2[0]);
        v16[1] = fm(v16[1], v13[0]);
        v16[3] = fm(v16[3], v13[1]);
        x->q = q2;
    }
    if (x->vi[5] == 0u) goto done;
    memcpy(v22, F(24), 16);
    memcpy(v23, F(25), 16);
    rows4(F(18), x, 9, v22);
    rows4(F(19), x, 9, v23);
    clipw(x, F(18));
    clipw(x, F(19));
    v22[3] = 1.0f;
    v23[3] = fm(1.0f, 0.0f);
    x->vi[1] = (x->clip & 0x000FFFu) ? 1u : 0u;
    if (x->vi[1]) {
        static const u32 planes[6] = { 0xFFFFBEu, 0xFFFF7Du, 0xFFFEFBu, 0xFFFDF7u,
                                       0xFFFBEFu, 0xFFF7DFu };
        for (int k = 0; k < 6; k++)
            if (((x->clip | planes[k]) & 0xFFFFFFu) == 0xFFFFFFu) {
                x->vi[1] = k < 5 ? (((x->clip | planes[k + 1]) & 0xFFFFFFu) == 0xFFFFFFu) : 1u;
                st_pt_rejected++;
                goto done;
            }
        x->vi[1] = 0;
        st_pt_clipped++;
        if (!fx1_clip_line(x, top)) { st_pt_rejected++; goto done; }
    }
    v27[0] = fa(0.0f, v22[3]);
    v27[1] = fa(0.0f, v23[3]);
    v22[3] = 1.0f;
    v23[3] = 1.0f;
    v2[0] = fs(v27[0], v27[1]);
    vp_lq(x, 1, 0x1Au, VP_MALL);
    v27[2] = fa(0.0f, v2[0]);
    rows4(v22, x, 5, v22);
    v2[3] = fm(v22[3], v1[1]);
    q = ps2_fdiv(1.0f, v22[3]);
    rows4(v23, x, 5, v23);
    for (int i = 0; i < 3; i++) v22[i] = fm(v22[i], q);
    v28[3] = fm(v16[3], q);
    v3[3] = fm(v23[3], v1[1]);
    q = ps2_fdiv(1.0f, v23[3]);
    v2[3] = fa(v2[3], v1[0]);
    v3[3] = fa(v3[3], v1[0]);
    v28[1] = fm(v16[1], v2[3]);
    vp_lq(x, 15, top + 5u, VP_MALL);
    for (int i = 0; i < 3; i++) v23[i] = fm(v23[i], q);
    v28[2] = fm(v16[2], q);
    v28[0] = fm(v16[0], v3[3]);
    q = ps2_fdiv(1.0f, v27[2]);
    {   int far0 = neg_nz(fs(v15[0], v22[3])), far1;
        v13[3] = fs(v22[3], v15[1]);
        if (!far0) { v13[3] = fm(v13[3], v15[2]); v28[1] = fm(v28[1], v13[3]); }
        far1 = neg_nz(fs(v15[0], v23[3]));
        v13[3] = fs(v23[3], v15[1]);
        if (!far1) { v13[3] = fm(v13[3], v15[2]); v28[0] = fm(v28[0], v13[3]); }
    }
    v28[0] = vmax(v28[0], 0.0f);
    v28[1] = vmax(v28[1], 0.0f);
    {   int nx = neg_nz(fs(v28[0], 1.0f)), ny = neg_nz(fs(v28[1], 1.0f));
        x->vi[1] = 0xC0;
        x->vi[2] = (u16)((nx ? 0x80u : 0u) | (ny ? 0x40u : 0u));
        v22[3] = 1.0f;
        v23[3] = 1.0f;
        v27[2] = fa(0.0f, q);
        x->q = q;
        if (x->vi[2] == x->vi[1]) { st_pt_faded++; goto done; }
    }
    vp_lq(x, 1, 0x1Cu, VP_MALL);
    vp_lq(x, 2, top + 0u, VP_MALL);
    vp_lq(x, 3, top + 1u, VP_MALL);
    vp_sq(x, x->vi[7] + 0u, v1, VP_MALL);
    vp_sq(x, x->vi[7] + 1u, v2, VP_MALL);
    vp_sq(x, x->vi[7] + 2u, v3, VP_MALL);
    x->vi[8] = (u16)(x->vi[8] + 3u);
    {   int nz = neg_nz(fs(v28[2], 2.0f)), nw = neg_nz(fs(v28[3], 2.0f));
        x->vi[1] = (u16)((nz ? 0x20u : 0u) | (nw ? 0x10u : 0u));
    }
    if (x->vi[1]) {
        st_pt_lines++;
        v15[2] = fm(v28[2], -1.0f); v15[3] = fm(v28[3], -1.0f);
        v15[2] = fa(v15[2], 2.0f);  v15[3] = fa(v15[3], 2.0f);
        v15[2] = vmin(v15[2], 1.0f); v15[3] = vmin(v15[3], 1.0f);
        v15[2] = vmax(v15[2], 0.0f); v15[3] = vmax(v15[3], 0.0f);
        v15[0] = fm(v28[0], v15[2]);
        v15[1] = fm(v28[1], v15[3]);
        for (int i = 0; i < 3; i++) v13[i] = vp_lane_ftoi(v22[i], 4);
        vp_lq(x, 2, top + 6u, VP_MALL);
        for (int i = 0; i < 3; i++) v14[i] = vp_lane_ftoi(v23[i], 4);
        vp_lq(x, 3, top + 6u, VP_MALL);
        v13[3] = 1.0f;
        vp_lq(x, 1, 0x1Eu, VP_MALL);
        v14[3] = 1.0f;
        v2[3] = fm(1.0f, v15[0]);
        v3[3] = fm(1.0f, v15[1]);
        v2[3] = fm(v2[3], imm(0x3F666666u));
        v3[3] = fm(v3[3], imm(0x3F666666u));
        v2[3] = vp_lane_ftoi(v2[3], 0);
        v3[3] = vp_lane_ftoi(v3[3], 0);
        vp_sq(x, x->vi[8] + 0u, v1, VP_MALL);
        vp_sq(x, x->vi[8] + 1u, v2, VP_MALL);
        vp_sq(x, x->vi[8] + 2u, v14, VP_MALL);
        vp_sq(x, x->vi[8] + 3u, v3, VP_MALL);
        vp_sq(x, x->vi[8] + 4u, v13, VP_MALL);
        x->vi[8] = (u16)(x->vi[8] + 5u);
    }
    {   int nz = neg_nz(fs(v28[2], 1.0f)), nw = neg_nz(fs(v28[3], 1.0f));
        x->vi[1] = 0x30;
        x->vi[2] = (u16)((nz ? 0x20u : 0u) | (nw ? 0x10u : 0u));
    }
    if (x->vi[2] != x->vi[1]) {
        u16 mac;
        float r[4];
        for (int i = 0; i < 4; i++) v13[i] = fs(v22[i], v23[i]);
        vp_lq(x, 2, 0x1Bu, VP_MALL);
        v14[1] = fs(v28[1], v28[0]);
        v14[3] = fs(v28[3], v28[2]);
        v14[0] = fs(v17[0], v17[1]);
        v1[2] = 0.0f;
        v1[3] = fm(1.0f, 0.0f);
        v1[0] = fm(v13[0], v2[2]);
        v1[1] = fm(v13[1], v2[3]);
        for (int i = 0; i < 3; i++) v13[i] = fm(v13[i], v27[2]);
        vp_lq(x, 2, top + 3u, VP_MALL);
        v4[2] = vmin(v28[2], v28[3]);
        x->p = eleng(v1);
        v4[2] = fm(v4[2], v2[2]);
        for (int i = 0; i < 3; i++) v2[i] = fm(v13[i], v27[1]);
        for (int i = 0; i < 3; i++) v23[i] = fs(v23[i], v2[i]);
        v4[0] = x->p;
        v4[0] = fm(v4[0], v27[2]);
        q = ps2_fdiv(v4[0], v4[2]);
        x->q = q;
        v2[0] = imm(0x3F4CCCCDu); v2[1] = imm(0x3FE66666u);
        v2[2] = imm(0x40733333u); v2[3] = fm(1.0f, imm(0x40F9999Au));
        v4[0] = fa(0.0f, q);
        v3[3] = fm(v3[3], 0.0f);
        v4[0] = vabs(v4[0]);
        for (int i = 0; i < 4; i++) r[i] = fs(v2[i], v4[0]);
        mac = mac4(r, 0xF);
        if (mac & 0x10u)      { v27[3] = fa(v3[3], 0.0625f); v1[0] = 15.0f;
                                x->vi[2] = mac & 0x10u; x->vi[3] = 0x20; }
        else if (mac & 0x20u) { v27[3] = fa(v3[3], 0.125f);  v1[0] = 7.0f;
                                x->vi[3] = mac & 0x20u; x->vi[2] = 0x40; }
        else if (mac & 0x40u) { v27[3] = fa(v3[3], 0.25f);   v1[0] = 3.0f;
                                x->vi[2] = mac & 0x40u; x->vi[3] = 0x80; }
        else if (mac & 0x80u) { v27[3] = fa(v3[3], 0.5f);    v1[0] = 1.0f;
                                x->vi[3] = mac & 0x80u; x->vi[2] = 0; }
        else                  { v27[3] = fa(v3[3], 1.0f);    v1[0] = 0.0f;
                                x->vi[2] = 0; x->vi[3] = 0; }
        x->vi[1] = mac;
        st_pt_steps[(mac & 0x10u) ? 0 : (mac & 0x20u) ? 1 : (mac & 0x40u) ? 2 : (mac & 0x80u) ? 3 : 4]++;
        v4[0] = fm(v4[0], v27[3]);
        v1[1] = -1.0f;
        x->vi[4] = 0xFFFFu;
        v4[0] = vmin(v4[0], 1.0f);
        {   int low = neg_nz(fs(v4[0], imm(0x3F333333u)));
            x->vi[1] = low ? 0x80u : 0u;
            v17[2] = fa(0.0f, v4[0]);
            if (low) {
                v4[0] = fm(v4[0], 3.5f);
                v4[0] = fs(v4[0], 1.75f);
                v4[0] = vmax(v4[0], 0.0f);
                v17[2] = fa(0.0f, v4[0]);
            }
        }
        fx1_sprites(x, top);
    }
    vp_lq(x, 1, 0x1Fu, VP_MALL);
    x->vi[1] = 0x190;
    vp_sq(x, x->vi[8], v1, VP_MALL);
    for (u16 a = x->vi[7]; ; a = (u16)(a + 1u)) {
        float t[4];
        vp_rd(x, a, t);
        pkt_putf(o++, t);
        if (a == x->vi[8]) break;
    }
    pkt_kick(x->vi[7]);
    st_pt_packets++;
    x->vi[7] = x->vi[7] == 0x190u ? 0x2BC : 0x190;
    x->vi[8] = x->vi[7];
done:
    v17[1] = fa(0.0f, v17[0]);
    vp_isw(x, 0x18u, 0, x->vi[7]);
    mr32(v16);
    memcpy(F(25), F(24), 16);
    x->vi[5] = (u16)(x->vi[5] + 1u);
    return o;
}

int rn_vp_fx1(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, o = 0;
    u16 count;
    (void)kick_at;
    if (pc == 0x0000u) {
        for (int k = 0; k < 8; k++) vp_lq(x, 5 + k, 0x10u + (u32)k, VP_MALL);
        x->tpc = 0x0050u;
        return 0;
    }
    if (pc != 0x0050u) return -1;
    top = x->top & VP_QW_MASK;
    count = vp_ilw(x, top + 2u, 0);
    if (count == 0u || count > 256u) { st_pt_declined++; return -1; }
    st_pt_batches++;
    x->vi[9] = (u16)top;
    x->vi[7] = vp_ilw(x, 0x18u, 0);
    x->vi[10] = (u16)(top + 0x34u);
    x->vi[1] = (u16)(top + 2u);
    x->vi[6] = count;
    x->vi[5] = 0;
    x->vi[8] = x->vi[7];
    F(26)[0] = -1.0f;
    do o = fx1_particle(x, top, o); while (x->vi[5] != x->vi[6]);
    x->tpc = 0x0050u;
    return o ? 1 : 0;
}

static u64 st_rain_batches, st_rain_drops, st_rain_drawn[3], st_rain_pieces, st_rain_declined;

static float mfir(u16 v) { s32 k = (s32)(s16)v; float f; memcpy(&f, &k, 4); return f; }

static void rain_place(vpx *x, float sy, u32 off, u16 ymask, float cy) {
    float *v1 = F(1), *v19 = F(19);
    v19[0] = fm(v19[0], 4095.0f); v19[2] = fm(v19[2], 4095.0f);
    v19[1] = fm(v19[1], sy);
    vp_lq(x, 1, off, VP_MALL);
    for (int i = 0; i < 3; i++) v19[i] = fa(v19[i], v1[i]);
    for (int i = 0; i < 3; i++) v19[i] = vp_lane_ftoi(v19[i], 0);
    v19[3] = 1.0f;
    x->vi[1] = (u16)(mtir(v19[0]) & 0xFFFu);
    x->vi[2] = (u16)(mtir(v19[1]) & ymask);
    x->vi[3] = (u16)(mtir(v19[2]) & 0xFFFu);
    x->vi[4] = ymask;
    v19[0] = mfir(x->vi[1]);
    v19[1] = mfir(x->vi[2]);
    v19[2] = mfir(x->vi[3]);
    for (int i = 0; i < 3; i++) v19[i] = vp_lane_itof(v19[i], 0);
    v19[0] = fs(v19[0], 2048.0f); v19[2] = fs(v19[2], 2048.0f);
    v19[1] = fs(v19[1], cy);
}

static int rain_inside(vpx *x, float *d, const float *w) {
    const float *v3 = F(3);
    int out;
    d[0] = fa(0.0f, v3[0]); d[1] = fa(0.0f, v3[0]);
    d[2] = fa(d[2], v3[1]); d[3] = fa(d[3], v3[1]);
    out = neg_nz(fs(d[0], w[0])) || neg_nz(fs(d[2], w[2]));
    x->vi[1] = 0x50;
    if (out) return 0;
    return !(neg_nz(fs(w[1], d[1])) || neg_nz(fs(w[3], d[3])));
}

static u32 rain_kick(vpx *x, u32 o) {
    float t[4];
    vp_lq(x, 1, 0x20u, VP_MALL);
    x->vi[1] = 0x258;
    vp_sq(x, x->vi[8], F(1), VP_MALL);
    for (u16 a = x->vi[7]; ; a = (u16)(a + 1u)) {
        vp_rd(x, a, t);
        pkt_putf(o++, t);
        if (a == x->vi[8]) break;
    }
    pkt_kick(x->vi[7]);
    x->vi[7] = x->vi[7] == 0x258u ? 0x320 : 0x258;
    x->vi[8] = x->vi[7];
    return o;
}

static void rain_fade(float *v4, const float *v3, int far, u32 kfar, u32 knear) {
    if (far) {
        v4[3] = fm(v4[3], imm(kfar));
        v4[3] = fa(v4[3], 2.0f);
        v4[3] = fm(v4[3], v3[0]);
    } else {
        v4[3] = fm(v4[3], imm(knear));
        v4[3] = fm(v4[3], v3[0]);
    }
}

static u32 rain_a(vpx *x, u32 top, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v20 = F(20), *v21 = F(21);
    float *v22 = F(22), *v23 = F(23), q;
    int lt, lt16;
    rain_place(x, 4095.0f, top + 0x19u, 0xFFF, 2048.0f);
    for (int k = 0; k < 4; k++) vp_lq(x, 15 + k, top + 6u + (u32)k, VP_MALL);
    rows4(v20, x, 7, F(19));
    vp_lq(x, 3, top + 0x13u, VP_MALL);
    rows4(v21, x, 15, F(19));
    q = ps2_fdiv(1.0f, v20[3]);
    v4[3] = fs(v20[3], v3[1]);
    lt = neg_nz(fs(v4[3], imm(0x45999000u)));
    for (int i = 0; i < 3; i++) v20[i] = fm(v20[i], q);
    q = ps2_fdiv(1.0f, v21[3]);
    x->q = q;
    lt16 = neg_nz(fs(v4[3], 16.0f));
    if (!lt) return o;
    for (int i = 0; i < 3; i++) v21[i] = fm(v21[i], q);
    lt = neg_nz(fs(v4[3], imm(0x45199000u)));
    if (lt16) return o;
    rain_fade(v4, v3, !lt, 0xB9D562ABu, 0x39D562ABu);
    if (neg_nz(fs(v4[3], 8.0f))) return o;
    v2[2] = 0.0f; v2[3] = fm(1.0f, 0.0f);
    vp_lq(x, 1, top + 0x14u, VP_MALL);
    v3[0] = fm(v21[0], 16.0f); v3[1] = fm(v21[1], 16.0f);
    v20[2] = fm(v20[2], 4.0f);
    v21[2] = fm(v21[2], 4.0f);
    if (!rain_inside(x, v2, v1)) return o;
    if (neg_nz(fs(v21[3], 16.0f))) return o;
    st_rain_drawn[0]++;
    for (int i = 0; i < 3; i++) v20[i] = vp_lane_ftoi(v20[i], 4);
    vp_lq(x, 1, 0x1Cu, VP_MALL);
    for (int i = 0; i < 3; i++) v21[i] = vp_lane_ftoi(v21[i], 4);
    vp_lq(x, 2, top + 0u, VP_MALL);
    vp_lq(x, 3, top + 1u, VP_MALL);
    vp_lq(x, 22, top + 0x16u, VP_MALL);
    vp_lq(x, 23, top + 0x16u, VP_MALL);
    vp_sq(x, x->vi[8] + 0u, v1, VP_MALL);
    vp_sq(x, x->vi[8] + 1u, v2, VP_MALL);
    vp_sq(x, x->vi[8] + 2u, v3, VP_MALL);
    v22[3] = fm(1.0f, v4[3]);
    x->vi[8] = (u16)(x->vi[8] + 3u);
    v23[3] = fm(1.0f, 0.0f);
    v20[3] = 1.0f;
    v21[3] = 1.0f;
    v22[3] = vp_lane_ftoi(v22[3], 0);
    v23[3] = vp_lane_ftoi(v23[3], 0);
    vp_lq(x, 1, 0x1Eu, VP_MALL);
    vp_sq(x, x->vi[8] + 2u, v20, VP_MALL);
    vp_sq(x, x->vi[8] + 4u, v21, VP_MALL);
    vp_sq(x, x->vi[8] + 1u, v22, VP_MALL);
    vp_sq(x, x->vi[8] + 3u, v23, VP_MALL);
    vp_sq(x, x->vi[8] + 0u, v1, VP_MALL);
    x->vi[8] = (u16)(x->vi[8] + 5u);
    return rain_kick(x, o);
}

static u32 rain_b(vpx *x, u32 top, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v5 = F(5), *v6 = F(6);
    float *v20 = F(20), *v21 = F(21), *v22 = F(22), *v23 = F(23), *v28 = F(28), q;
    int behind, lt, small, fail16;
    rain_place(x, 2047.0f, top + 0x1Au, 0x7FF, 1024.0f);
    for (int k = 0; k < 4; k++) vp_lq(x, 15 + k, top + 0x0Au + (u32)k, VP_MALL);
    rows4(v20, x, 7, F(19));
    vp_lq(x, 3, top + 0x13u, VP_MALL);
    rows4(v21, x, 15, F(19));
    q = ps2_fdiv(1.0f, v20[3]);
    v4[3] = fs(v20[3], v3[1]);
    behind = neg_nz(fs(v4[3], 0.0f));
    for (int i = 0; i < 3; i++) v20[i] = fm(v20[i], q);
    q = ps2_fdiv(1.0f, v21[3]);
    lt = neg_nz(fs(v4[3], 2048.0f));
    x->q = q;
    if (behind) return o;
    for (int i = 0; i < 3; i++) v21[i] = fm(v21[i], q);
    q = ps2_fdiv(1.0f, v4[3]);
    x->q = q;
    rain_fade(v4, v3, !lt, 0xBA000000u, 0x3A000000u);
    v4[0] = fa(0.0f, q);
    if (neg_nz(fs(v4[3], 8.0f))) return o;
    v2[2] = 0.0f; v2[3] = fm(1.0f, 0.0f);
    vp_lq(x, 1, top + 0x14u, VP_MALL);
    v3[0] = fm(v20[0], 16.0f); v3[1] = fm(v20[1], 16.0f);
    vp_lq(x, 5, top + 0x15u, VP_MALL);
    v20[2] = fm(v20[2], v5[2]);
    v4[0] = fm(v4[0], v5[0]);
    if (!rain_inside(x, v2, v1)) return o;
    if (neg_nz(fs(v20[3], 16.0f))) return o;
    v1[0] = fs(v21[0], v20[0]); v1[1] = fs(v21[1], v20[1]);
    v1[2] = fm(0.0f, 0.0f); v1[3] = fm(1.0f, 0.0f);
    v2[0] = fm(v4[0], 0.03125f);
    v2[1] = fa(0.0f, 4.0f);
    x->p = eleng(v1);
    q = ps2_fdiv(1.0f, v2[0]);
    v2[1] = fm(v2[1], v2[0]);
    v2[3] = fm(v4[3], q);
    v1[2] = x->p;
    small = neg_nz(fs(v1[2], v2[0]));
    q = ps2_fdiv(1.0f, v1[2]);
    x->q = q;
    if (small) {
        v4[3] = fm(v2[3], v1[2]);
        if (neg_nz(fs(v4[3], 8.0f))) return o;
    }
    v1[0] = fm(v1[0], q); v1[1] = fm(v1[1], q);
    if (neg_nz(fs(v1[2], v2[1]))) v1[2] = fa(0.0f, v2[1]);
    v5[0] = fm(v1[0], v1[2]); v5[1] = fm(v1[1], v1[2]);
    v21[0] = fa(v20[0], v5[0]); v21[1] = fa(v20[1], v5[1]);
    v5[2] = 0.0f; v5[3] = fm(1.0f, 0.0f);
    vp_lq(x, 6, top + 0x14u, VP_MALL);
    v3[0] = fm(v21[0], 16.0f); v3[1] = fm(v21[1], 16.0f);
    if (!rain_inside(x, v5, v6)) return o;
    vp_lq(x, 6, top + 0x15u, VP_MALL);
    fail16 = neg_nz(fs(v21[3], 16.0f));
    v21[2] = fm(v21[2], v6[2]);
    if (fail16) return o;
    st_rain_drawn[1]++;
    vp_lq(x, 6, 0x1Cu, VP_MALL);
    vp_lq(x, 5, top + 0u, VP_MALL);
    vp_lq(x, 3, top + 1u, VP_MALL);
    vp_sq(x, x->vi[8] + 0u, v6, VP_MALL);
    vp_sq(x, x->vi[8] + 1u, v5, VP_MALL);
    vp_sq(x, x->vi[8] + 2u, v3, VP_MALL);
    x->vi[8] = (u16)(x->vi[8] + 3u);
    vp_lq(x, 6, 0x1Du, VP_MALL);
    vp_lq(x, 5, top + 0x16u, VP_MALL);
    vp_sq(x, x->vi[8] + 0u, v6, VP_MALL);
    v5[3] = fm(1.0f, v4[3]);
    v5[3] = vp_lane_ftoi(v5[3], 0);
    for (u32 k = 0; k < 4; k++) vp_sq(x, x->vi[8] + 2u + 3u * k, v5, VP_MALL);
    v1[0] = fm(v1[0], v4[0]); v1[1] = fm(v1[1], v4[0]);
    vp_lq(x, 3, 0x1Bu, VP_MALL);
    for (int i = 0; i < 3; i++) v20[i] = vp_lane_ftoi(v20[i], 4);
    for (int i = 0; i < 3; i++) v21[i] = vp_lane_ftoi(v21[i], 4);
    v1[1] = fm(v1[1], v3[0]);
    v1[0] = fm(v1[0], v3[1]);
    for (int i = 0; i < 3; i++) v20[i] = vp_lane_itof(v20[i], 0);
    for (int i = 0; i < 3; i++) v21[i] = vp_lane_itof(v21[i], 0);
    v22[0] = fa(v21[0], v1[1]); v22[1] = fs(v21[1], v1[0]);
    v23[0] = fs(v21[0], v1[1]); v23[1] = fa(v21[1], v1[0]);
    v21[0] = fs(v20[0], v1[1]); v21[1] = fa(v20[1], v1[0]);
    v20[0] = fa(v20[0], v1[1]); v20[1] = fs(v20[1], v1[0]);
    v23[2] = fa(v21[2], 0.0f);
    v22[2] = fa(v21[2], 0.0f);
    v21[2] = fa(v20[2], 0.0f);
    v20[3] = v21[3] = v22[3] = v23[3] = fm(1.0f, 0.0f);
    for (int k = 20; k < 24; k++)
        for (int i = 0; i < 4; i++) F(k)[i] = vp_lane_ftoi(F(k)[i], 0);
    for (u32 k = 0; k < 4; k++) vp_sq(x, x->vi[8] + 3u + 3u * k, F(20 + (int)k), VP_MALL);
    vp_lq(x, 3, top + 0x17u, VP_MALL);
    v1[3] = fm(v3[3], v28[0]);
    v1[1] = fm(v28[1], 7.0f);
    v1[1] = vp_lane_ftoi(v1[1], 0); v1[3] = vp_lane_ftoi(v1[3], 0);
    v1[1] = vp_lane_itof(v1[1], 0); v1[3] = vp_lane_itof(v1[3], 0);
    v1[2] = fm(v3[2], v1[3]);
    v1[1] = fm(v1[1], 4.0f);
    v3[0] = fa(v3[0], v1[2]);
    v1[1] = vp_lane_ftoi(v1[1], 0);
    x->vi[1] = (u16)(mtir(v1[1]) + 0xA2u);
    for (int k = 0; k < 4; k++) vp_lq(x, 20 + k, x->vi[1] + (u32)k, VP_MALL);
    for (int k = 20; k < 24; k++) { F(k)[0] = fm(F(k)[0], v3[2]); F(k)[1] = fm(F(k)[1], v3[2]); }
    for (int k = 20; k < 24; k++) { F(k)[0] = fa(F(k)[0], v3[0]); F(k)[1] = fa(F(k)[1], v3[1]); }
    for (int k = 20; k < 24; k++) {
        F(k)[0] = vp_lane_ftoi(F(k)[0], 0); F(k)[1] = vp_lane_ftoi(F(k)[1], 0);
    }
    for (u32 k = 0; k < 4; k++) vp_sq(x, x->vi[8] + 1u + 3u * k, F(20 + (int)k), VP_MALL);
    x->vi[8] = (u16)(x->vi[8] + 0x0Du);
    return rain_kick(x, o);
}

static u32 rain_c(vpx *x, u32 top, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v4 = F(4), *v5 = F(5), *v6 = F(6);
    float *v20 = F(20), *v21 = F(21), *v22 = F(22), *v23 = F(23), *v28 = F(28), *v29 = F(29);
    float q;
    int lt;
    u16 *vi = x->vi;
    rain_place(x, 4095.0f, top + 0x1Bu, 0xFFF, 2048.0f);
    vp_lq(x, 2, top + 0x15u, VP_MALL);
    for (int i = 0; i < 3; i++) v1[i] = fs(v28[i], 0.5f);
    v4[0] = fa(0.0f, v2[1]);
    v2[3] = fm(v2[3], 2.0f);
    for (int i = 0; i < 3; i++) v1[i] = fm(v1[i], v2[3]);
    for (int i = 0; i < 3; i++) F(19)[i] = fa(F(19)[i], v1[i]);
    rows4(v20, x, 7, F(19));
    vp_lq(x, 3, top + 0x13u, VP_MALL);
    v4[3] = fs(v20[3], v3[1]);
    q = ps2_fdiv(1.0f, v20[3]);
    x->q = q;
    v1[0] = vabs(F(19)[0]); v1[1] = vabs(F(19)[1]);
    lt = neg_nz(fs(v4[3], 2048.0f));
    for (int i = 0; i < 3; i++) v20[i] = fm(v20[i], q);
    v4[0] = fm(v4[0], q);
    rain_fade(v4, v3, !lt, 0xBA000000u, 0x3A000000u);
    if (!neg_nz(fs(v1[0], 1024.0f))) {
        v1[0] = fm(v1[0], imm(0xBA800000u));
        v1[0] = fa(v1[0], 2.0f);
        v4[3] = fm(v4[3], v1[0]);
    }
    if (!neg_nz(fs(v1[1], 1024.0f))) {
        v1[1] = fm(v1[1], imm(0xBA800000u));
        v1[1] = fa(v1[1], 2.0f);
        v4[3] = fm(v4[3], v1[1]);
    }
    if (neg_nz(fs(v4[3], 4.0f))) return o;
    v2[2] = 0.0f; v2[3] = fm(1.0f, 0.0f);
    vp_lq(x, 1, top + 0x14u, VP_MALL);
    v3[0] = fm(v20[0], 16.0f); v3[1] = fm(v20[1], 16.0f);
    v20[2] = fm(v20[2], 4.0f);
    if (!rain_inside(x, v2, v1)) return o;
    if (neg_nz(fs(v20[3], 16.0f))) return o;
    st_rain_drawn[2]++;
    for (int i = 0; i < 4; i++) v20[i] = vp_lane_ftoi(v20[i], 4);
    vp_lq(x, 1, 0x1Cu, VP_MALL);
    vp_lq(x, 2, top + 0u, VP_MALL);
    vp_lq(x, 3, top + 1u, VP_MALL);
    vp_lq(x, 5, 0x1Bu, VP_MALL);
    for (int i = 0; i < 4; i++) v20[i] = vp_lane_itof(v20[i], 0);
    vp_sq(x, vi[8] + 0u, v1, VP_MALL);
    vp_sq(x, vi[8] + 1u, v2, VP_MALL);
    vp_sq(x, vi[8] + 2u, v3, VP_MALL);
    vi[8] = (u16)(vi[8] + 3u);
    v1[0] = fm(v5[0], v4[0]); v1[1] = fm(v5[1], v4[0]);
    vp_lq(x, 29, top + 0x16u, VP_MALL);
    v29[3] = fm(1.0f, v4[3]);
    v2[0] = fs(v20[0], v1[0]); v2[1] = fs(v20[1], v1[1]);
    v3[0] = fa(v20[0], v1[0]); v3[1] = fa(v20[1], v1[1]);
    v4[0] = vp_lane_ftoi(v2[0], 0);
    v20[1] = fa(0.0f, v2[1]);
    vp_lq(x, 6, top + 0x18u, VP_MALL);
    v21[1] = fa(0.0f, v3[1]);
    v21[2] = fa(0.0f, v20[2]);
    v1[0] = fs(v3[0], v2[0]);
    vi[2] = mtir(v4[0]);
    v5[0] = fm(v28[0], v6[3]);
    v20[3] = fm(1.0f, 0.0f);
    v21[3] = fm(1.0f, 0.0f);
    vi[12] = 0x3FF;
    v4[0] = vp_lane_ftoi(v3[0], 0);
    q = ps2_fdiv(1.0f, v1[0]);
    x->q = q;
    v5[0] = vp_lane_ftoi(v5[0], 0);
    vi[1] = (u16)(vi[2] & vi[12]);
    v29[3] = vp_lane_ftoi(v29[3], 0);
    vi[12] = 0x400;
    v22[1] = fa(0.0f, v6[1]);
    vi[3] = (u16)(vi[12] - vi[1]);
    v23[1] = fa(v6[1], v6[2]);
    vi[13] = mtir(v4[0]);
    vi[12] = (u16)(vi[2] - vi[1]);
    v5[0] = vp_lane_itof(v5[0], 0);
    vi[13] = (u16)(vi[13] - vi[12]);
    vi[13] = (u16)(vi[13] + 0x3FFu);
    v5[2] = fm(v6[2], q);
    v4[1] = mfir(vi[13]);
    v1[0] = vp_lane_ftoi(v1[0], 0);
    v5[0] = fm(v5[0], v6[2]);
    v20[1] = vp_lane_ftoi(v20[1], 0); v20[2] = vp_lane_ftoi(v20[2], 0);
    v21[1] = vp_lane_ftoi(v21[1], 0); v21[2] = vp_lane_ftoi(v21[2], 0);
    v4[1] = vp_lane_itof(v4[1], 0);
    vi[4] = mtir(v1[0]);
    v6[0] = fa(v6[0], v5[0]);
    v4[1] = fm(v4[1], imm(0x3A800000u));
    v5[0] = vp_lane_ftoi(v6[0], 0);
    v22[1] = vp_lane_ftoi(v22[1], 0);
    v23[1] = vp_lane_ftoi(v23[1], 0);
    v4[1] = vp_lane_ftoi(v4[1], 0);
    vi[6] = mtir(v5[0]);
    vi[1] = 0;
    vi[12] = 0;
    vp_lq(x, 6, 0x1Fu, VP_MALL);
    vi[13] = mtir(v4[1]);
    do {
        vi[14] = (u16)(vi[3] - vi[4]);
        vp_sq(x, vi[8] + 0u, v6, VP_MALL);
        if ((s16)vi[14] > 0) vi[3] = vi[4];
        vi[4] = (u16)(vi[4] - vi[3]);
        vi[1] = vi[2];
        vi[2] = (u16)(vi[2] + vi[3]);
        vi[6] = (u16)(vi[6] + vi[12]);
        v5[0] = mfir(vi[3]);
        vp_sq(x, vi[8] + 1u, v29, VP_MALL);
        v20[0] = mfir(vi[1]);
        v21[0] = mfir(vi[2]);
        v5[0] = vp_lane_itof(v5[0], 0);
        vp_sq(x, vi[8] + 3u, v20, VP_MALL);
        vp_sq(x, vi[8] + 5u, v21, VP_MALL);
        v1[2] = fm(v5[2], v5[0]);
        v1[2] = vp_lane_ftoi(v1[2], 0);
        vi[12] = mtir(v1[2]);
        vi[3] = 0x400;
        v22[0] = mfir(vi[6]);
        vi[14] = (u16)(vi[6] + vi[12]);
        v23[0] = mfir(vi[14]);
        vp_sq(x, vi[8] + 2u, v22, VP_MALL);
        vp_sq(x, vi[8] + 4u, v23, VP_MALL);
        vi[13] = (u16)(vi[13] - 1u);
        vi[8] = (u16)(vi[8] + 6u);
        st_rain_pieces++;
    } while ((s16)vi[13] > 0);
    return rain_kick(x, o);
}

int rn_vp_rain(vpx *x, u32 pc, u32 *kick_at) {
    float *v1 = F(1), *v2 = F(2), *v19 = F(19), *v26 = F(26), *v27 = F(27), last;
    u32 top, o = 0;
    (void)kick_at;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u) return -1;
    top = x->top & VP_QW_MASK;
    last = vp_rd_lane(x, top + 0x12u, 3);
    if (!(last <= 180.0f)) { st_rain_declined++; return -1; }
    st_rain_batches++;
    x->vi[9] = (u16)top;
    x->vi[7] = 0x258;
    x->vi[8] = x->vi[7];
    x->vi[10] = (u16)(top + 0x1Cu);
    v26[0] = -1.0f;
    vp_lq(x, 27, top + 0x12u, VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 7 + k, top + 2u + (u32)k, VP_MALL);
    for (int k = 0; k < 4; k++) vp_lq(x, 11 + k, top + 0x0Eu + (u32)k, VP_MALL);
    do {
        v26[0] = fa(v26[0], 1.0f);
        x->vi[10] = (u16)(x->vi[10] + 1u);
        v26[1] = -1.0f;
        x->vi[11] = (u16)(top + 0x1Cu);
        do {
            int a, b;
            st_rain_drops++;
            v26[1] = fa(v26[1], 1.0f);
            vp_lq(x, 2, x->vi[10], VP_MALL);
            vp_lq(x, 1, x->vi[11], VP_MALL);
            x->vi[11] = (u16)(x->vi[11] + 1u);
            for (int i = 0; i < 4; i++) v1[i] = fm(v1[i], v2[i]);
            rinit(x, v1[0]); v19[0] = rnext(x);
            rinit(x, v1[1]); v19[1] = rnext(x);
            rinit(x, v1[2]); v19[2] = rnext(x);
            a = neg_nz(fs(v26[1], v27[0]));
            for (int i = 0; i < 3; i++) v19[i] = fs(v19[i], 1.0f);
            b = neg_nz(fs(v26[1], v27[1]));
            if (a) {
                x->vi[1] = 0x40;
                o = rain_a(x, top, o);
            } else {
                for (int i = 0; i < 3; i++) F(28)[i] = fa(0.0f, v19[i]);
                x->vi[1] = b ? 0x40 : 0;
                o = b ? rain_b(x, top, o) : rain_c(x, top, o);
            }
        } while (neg_nz(fs(v26[1], v27[3])));
    } while (neg_nz(fs(v26[0], 4.0f)));
    x->vi[1] = 0x20;
    {   float t[4];
        vp_rd(x, 0x20u, t);
        pkt_putf(o++, t);
        pkt_kick(0x20u);
    }
    x->tpc = 0x0010u;
    return 1;
}

void rn_vp_fx_report(void) {
    if (st_rib_batches)
        ps2_log("rn: native ribbon program -- %llu batches, %llu triangles: %llu wholly "
                "inside, %llu clipped, %llu rejected by a plane, %llu clipped away",
                (unsigned long long)st_rib_batches, (unsigned long long)st_rib_tris,
                (unsigned long long)st_rib_inside, (unsigned long long)st_rib_clipped,
                (unsigned long long)st_rib_rejected, (unsigned long long)st_rib_degenerate);
    if (st_pt_batches | st_pt_declined)
        ps2_log("rn: native particle program -- %llu batches, %llu particles, %llu packets "
                "(%llu with a line, %llu sprites); %llu segments clipped, %llu rejected, "
                "%llu faded out; %llu batches left to VU1", (unsigned long long)st_pt_batches,
                (unsigned long long)st_pt_particles, (unsigned long long)st_pt_packets,
                (unsigned long long)st_pt_lines, (unsigned long long)st_pt_sprites,
                (unsigned long long)st_pt_clipped, (unsigned long long)st_pt_rejected,
                (unsigned long long)st_pt_faded, (unsigned long long)st_pt_declined);
    if (st_pt_batches)
        ps2_log("rn: native particle program -- ends cut %llu / %llu; sprite runs of "
                "16/8/4/2/1: %llu %llu %llu %llu %llu",
                (unsigned long long)st_pt_cut[0], (unsigned long long)st_pt_cut[1],
                (unsigned long long)st_pt_steps[0], (unsigned long long)st_pt_steps[1],
                (unsigned long long)st_pt_steps[2], (unsigned long long)st_pt_steps[3],
                (unsigned long long)st_pt_steps[4]);
    if (st_rain_batches | st_rain_declined)
        ps2_log("rn: native rain program -- %llu batches, %llu drops: %llu streaks, %llu "
                "quads, %llu splashes (%llu sprites); %llu batches left to VU1",
                (unsigned long long)st_rain_batches, (unsigned long long)st_rain_drops,
                (unsigned long long)st_rain_drawn[0], (unsigned long long)st_rain_drawn[1],
                (unsigned long long)st_rain_drawn[2], (unsigned long long)st_rain_pieces,
                (unsigned long long)st_rain_declined);
}
