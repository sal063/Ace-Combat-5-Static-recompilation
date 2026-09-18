#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

#define F(r) (x->vf[r].f)

static u64 st_lit_fast, st_lit_outside, st_lit_clip, st_lit_other, st_lit_culled;
static u64 st_init[1], st_batch_declined[1];

static int mac_sign(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) && (u & 0x7FFFFFFFu);
}
static float vmax(float a, float b) { return ps2_vu_select(a, b, 0); }
static float vmin(float a, float b) { return ps2_vu_select(a, b, 1); }

static u32 lit_emit(vpx *x, u32 top, u16 count, u32 o) {
    int culled = (x->vi[5] & 1u) != 0;
    u16 vi9 = count, vi12 = 0;
    u32 vi10 = top + 1u;
    float hdr[4];
    for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x24u + (u32)k, VP_MALL);
    vp_rd(x, culled ? 0x01u : 0x00u, hdr);
    {   u16 vi3 = (u16)(vp_ilw(x, 0x00u, 0) | vi9);
        u32 w[4];
        memcpy(w, hdr, 16);
        w[0] = vi3;
        pkt_put(o++, w);
    }
    do {
        float *v13 = F(13), *v14 = F(14), *v15 = F(15), *v1 = F(1), *v3 = F(3);
        float q, acc;
        vp_lq(x, 14, vi10 + 1u, VP_MXYZ);
        vp_lq(x, 13, vi10, VP_MXYZ);
        vp_lq(x, 15, 0x3Cu, VP_MALL);
        for (int i = 0; i < 3; i++) v14[i] = vp_lane_itof(v14[i], 12);
        if (culled) vp_lq(x, 3, 0x18u, VP_MALL);
        vp_rows(v13, F(5), F(6), F(7), F(8), v13[0], v13[1], v13[2], 1.0f, VP_MALL);
        for (int i = 0; i < 3; i++) {
            float a = F(17)[i] * v14[0];
            a = a + F(18)[i] * v14[1];
            v1[i] = a + F(19)[i] * v14[2];
        }
        v1[3] = 1.0f + 0.0f;
        q = ps2_fdiv(1.0f, v13[3]);
        vi10 += 5u;
        for (int i = 0; i < 3; i++) v13[i] = v13[i] * q;
        if (!culled) {
            vp_lq(x, 3, 0x18u, VP_MALL);
            acc = 1.0f * v3[0];
            for (int i = 0; i < 4; i++) v1[i] = vmax(v1[i], 0.0f);
            v13[3] = acc + v13[3] * v3[1];
            vp_rows(v1, F(21), F(22), F(23), F(24), v1[0], v1[1], v1[2], v1[3], VP_MALL);
            v13[2] = vmin(v13[2], 16777215.0f);
            v13[3] = vmin(v13[3], 255.0f);
            for (int i = 0; i < 4; i++) v1[i] = vmax(v1[i], 0.0f);
            for (int i = 0; i < 3; i++) v15[i] = v15[i] * v1[i];
            v15[3] = 1.0f * 128.0f;
            v13[2] = vmax(v13[2], 0.0f);
            v13[3] = vmax(v13[3], 0.0f);
            for (int i = 0; i < 3; i++) v15[i] = vmin(v15[i], 255.0f);
            {   float c[4], pre[4];
                memcpy(pre, v13, 16);
                for (int i = 0; i < 4; i++) {
                    s32 k = vp_ftoi(v15[i], 0);
                    memcpy(&c[i], &k, 4);
                    memcpy(&v15[i], &k, 4);
                }
                pkt_putf(o++, c);
                pkt_put_xyzf(o++, pre, vp_ftoi(pre[3], 4));
                for (int i = 0; i < 4; i++) {
                    s32 k = vp_ftoi(pre[i], 4);
                    memcpy(&v13[i], &k, 4);
                }
            }
        } else {
            float pre[4], c[4];
            acc = 1.0f * v3[0];
            v13[3] = acc + v13[3] * v3[1];
            v13[2] = vmin(v13[2], 16777215.0f);
            v13[3] = vmin(v13[3], 255.0f);
            for (int i = 0; i < 4; i++) v1[i] = vmax(v1[i], 0.0f);
            v13[2] = vmax(v13[2], 0.0f);
            v13[3] = vmax(v13[3], 0.0f);
            vp_rows(v1, F(21), F(22), F(23), F(24), v1[0], v1[1], v1[2], v1[3], VP_MALL);
            for (int i = 0; i < 4; i++) v1[i] = vmax(v1[i], 0.0f);
            for (int i = 0; i < 3; i++) v15[i] = v15[i] * v1[i];
            v15[3] = 1.0f * 128.0f;
            for (int i = 0; i < 3; i++) v15[i] = vmin(v15[i], 255.0f);
            for (int i = 0; i < 4; i++) F(29)[i] = v13[i] + 0.0f;
            memcpy(pre, v13, 16);
            for (int i = 0; i < 4; i++) {
                s32 k = vp_ftoi(v15[i], 0), m = vp_ftoi(v13[i], 4);
                memcpy(&v15[i], &k, 4);
                memcpy(&v13[i], &m, 4);
            }
            memcpy(c, v15, 16);
            {
                s32 wl = vp_ftoi(pre[3], 4);
                if (vi12 >= 2u) {
                    float e1[4], e2[4], z, a;
                    u16 vi3 = vp_ilw(x, 0x0Fu, 3);
                    int neg;
                    for (int i = 0; i < 4; i++) {
                        e1[i] = F(27)[i] - F(28)[i];
                        e2[i] = F(28)[i] - F(29)[i];
                    }
                    memcpy(F(1), e1, 16);
                    memcpy(F(2), e2, 16);
                    a = e1[0] * e2[1];
                    z = a - e2[0] * e1[1];
                    if (x->vi[14] == 0u) vi3 = (u16)(vi3 + vi12);
                    if (x->vi[5] & 0x80u) neg = !(vi3 & 1u);
                    else neg = (vi3 & 1u) != 0;
                    if (neg) z = z * -1.0f;
                    F(3)[2] = z;
                    if (!mac_sign(z)) {
                        wl = (s32)(s16)(u16)((u16)wl + 0x8000u);
                        memcpy(&v13[3], &wl, 4);
                        st_lit_culled++;
                    }
                }
                pkt_putf(o++, c);
                pkt_put_xyzf(o++, pre, wl);
            }
            for (int i = 0; i < 4; i++) {
                F(27)[i] = F(28)[i] + 0.0f;
                F(28)[i] = F(29)[i] + 0.0f;
            }
            vi12++;
        }
        vi9--;
    } while (vi9 != 0u);
    x->vi[9] = 0;
    x->vi[10] = (u16)vi10;
    if (culled) x->vi[12] = vi12;
    return o;
}

int rn_vp_lit(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, rec0, vi7, o = 0;
    u16 count, all = 0x7FFF, any = 0;
    int changed;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u) { st_lit_other++; return -1; }
    top = x->top & VP_QW_MASK;
    count = vp_ilw(x, top, 0);
    if (!count) { st_lit_other++; return -1; }
    rec0 = top + 1u;
    changed = (s16)vp_ilw(x, 0x1Bu, 2) > 0;
    {
        u16 flags = changed ? vp_ilw(x, 0x1Bu, 0) : x->vi[5];
        float C[4][4], B[4][4], A[4][4];
        if (flags & 0x20u) {
            if (changed) {
                for (int k = 0; k < 4; k++) { vp_rd(x, 0x28u + k, B[k]); vp_rd(x, 0x14u + k, A[k]); }
                for (int k = 0; k < 4; k++)
                    vp_rows(C[k], A[0], A[1], A[2], A[3], B[k][0], B[k][1], B[k][2], B[k][3], VP_MALL);
            } else {
                for (int k = 0; k < 4; k++) vp_rd(x, 0x30u + k, C[k]);
            }
            for (u32 k = 0; k < count; k++) {
                float p[4], c[4];
                u16 code;
                vp_rd(x, rec0 + 5u * k, p);
                vp_rows(c, C[0], C[1], C[2], C[3], p[0], p[1], p[2], 1.0f, VP_MALL);
                code = vp_clip_code(c);
                all &= code;
                any |= code;
            }
            if (!all && any) { st_lit_clip++; return -1; }
        } else {
            all = 0;
        }
    }

    if ((s16)vp_ilw(x, top, 1) <= 0) {
        for (u32 k = 0; k < count; k++) {
            u32 a = rec0 + 5u * k;
            float f[4];
            vp_rd(x, a + 1u, f);
            for (int i = 0; i < 3; i++) f[i] = vp_lane_ftoi(f[i], 12);
            vp_sq(x, a + 1u, f, VP_MXYZ);
            vp_rd(x, a + 2u, f);
            for (int i = 0; i < 4; i++) f[i] = vp_lane_ftoi(f[i], 0);
            vp_sq(x, a + 2u, f, VP_MALL);
        }
    }
    vi7 = top + 1u + 5u * count;
    x->vi[6] = count;
    x->vi[7] = (u16)vi7;
    x->vi[8] = (u16)vi7;
    if (changed) {
        float A[4][4];
        vp_isw(x, 0x1Bu, 2, 0);
        x->vi[5] = vp_ilw(x, 0x1Bu, 0);
        for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x28u + (u32)k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_rd(x, 0x10u + (u32)k, A[k]);
        for (int k = 0; k < 4; k++)
            vp_rows(F(5 + k), A[0], A[1], A[2], A[3], F(21 + k)[0], F(21 + k)[1],
                    F(21 + k)[2], F(21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_rd(x, 0x14u + (u32)k, A[k]);
        for (int k = 0; k < 4; k++)
            vp_rows(F(9 + k), A[0], A[1], A[2], A[3], F(21 + k)[0], F(21 + k)[1],
                    F(21 + k)[2], F(21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) vp_sq(x, 0x30u + (u32)k, F(9 + k), VP_MALL);
        for (int k = 0; k < 4; k++) vp_rd(x, 0x20u + (u32)k, A[k]);
        for (int k = 0; k < 4; k++)
            vp_rows(F(17 + k), A[0], A[1], A[2], A[3], F(21 + k)[0], F(21 + k)[1],
                    F(21 + k)[2], F(21 + k)[3], VP_MALL);
        for (int k = 0; k < 4; k++) memcpy(F(1 + k), A[k], 16);
    } else {
        for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x30u + (u32)k, VP_MALL);
    }

    if (all) {
        st_lit_outside++;
    } else {
        x->vi[9] = count;
        x->vi[10] = (u16)rec0;
        vp_isw(x, 0x0Fu, 3, (u16)rec0);
        x->vi[14] = 0;
        o = lit_emit(x, top, count, 0);
        st_lit_fast++;
    }
    {   float t[4];
        vp_rd(x, 0x06u, t);
        pkt_putf(o, t);
    }
    x->vi[8] = (u16)(vi7 + o);
    vp_isw(x, 0x3FFu, 0, x->vi[13]);
    vp_isw(x, 0x3FFu, 1, x->vi[9]);
    vp_isw(x, 0x3FFu, 2, x->vi[8]);
    vp_isw(x, 0x3FFu, 3, x->vi[7]);
    *kick_at = vi7;
    x->tpc = 0x0010u;
    return 1;
}

int rn_vp_skin(vpx *x, u32 pc, u32 *kick_at) {
    (void)kick_at;
    if (pc == 0x0000u) { x->tpc = 0x0010u; st_init[0]++; return 0; }
    st_batch_declined[0]++;
    return -1;
}

void rn_vp_lit_report(void) {
    if (st_lit_fast | st_lit_outside | st_lit_clip | st_lit_other)
        ps2_log("rn: native lit strip program -- %llu batches drawn natively (%llu "
                "triangles culled), %llu wholly outside; left to VU1: %llu needing the "
                "clipper, %llu other", (unsigned long long)st_lit_fast,
                (unsigned long long)st_lit_culled, (unsigned long long)st_lit_outside,
                (unsigned long long)st_lit_clip, (unsigned long long)st_lit_other);
    if (st_init[0] || st_batch_declined[0])
        ps2_log("rn: native VU1 skinned 3A1D40 -- %llu init entries native, %llu batches "
                "left to VU1", (unsigned long long)st_init[0],
                (unsigned long long)st_batch_declined[0]);
}
