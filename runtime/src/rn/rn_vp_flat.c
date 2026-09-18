#include "rn_vp_int.h"

#include <stdio.h>

#define F(r) (x->vf[r].f)
#define fa ps2_fadd
#define fs ps2_fsub
#define fm ps2_fmul

static u64 st_batches, st_runs, st_tris_clipped, st_fans, st_rejected, st_declined;

static int neg_nz(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) && (u & 0x7FFFFFFFu);
}
static float vabs(float a) { u32 u; memcpy(&u, &a, 4); u &= 0x7FFFFFFFu; memcpy(&a, &u, 4); return a; }
static float imm(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static float mfir(u16 v) { s32 k = (s32)(s16)v; float f; memcpy(&f, &k, 4); return f; }

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

static u16 clip_flags(vpx *x, const float *c) {
    float *v2 = F(2), *v3 = F(3), acc;
    u16 lo = 0, hi = 0;
    for (int i = 0; i < 3; i++) v3[i] = fa(c[i], c[3]);
    acc = fa(0.0f, c[3]);
    for (int i = 0; i < 3; i++) v2[i] = fs(acc, fm(c[i], 1.0f));
    for (int i = 0; i < 3; i++) if (neg_nz(v3[i])) lo |= (u16)(0x80u >> i);
    for (int i = 0; i < 2; i++) if (neg_nz(v2[i])) hi |= (u16)(0x80u >> i);
    x->vi[3] = hi;
    x->vi[2] = (u16)((lo << 3) | hi);
    return x->vi[2];
}

static u32 put_vertices(vpx *x, u32 o) {
    float t[4];
    (void)t;
    vp_lq(x, 1, x->vi[14], VP_MY | VP_MZ | VP_MW);
    vp_sq(x, x->vi[8], F(1), VP_MY | VP_MZ | VP_MW);
    vp_isw(x, x->vi[8], 0, x->vi[9]);
    x->vi[8] = (u16)(x->vi[8] + 1u);
    do {
        float *v13 = F(13), p[4], q;
        vp_lq(x, 13, x->vi[10], VP_MXYZ);
        p[0] = v13[0]; p[1] = v13[1]; p[2] = v13[2]; p[3] = 1.0f;
        rows4(v13, x, 5, p);
        x->vi[10] = (u16)(x->vi[10] + 5u);
        q = ps2_fdiv(1.0f, v13[3]);
        for (int i = 0; i < 3; i++) v13[i] = fm(v13[i], q);
        v13[2] = fm(v13[2], imm(0x3F83D70Au));
        v13[2] = ps2_vu_select(v13[2], imm(0x4B7FFFFFu), 1);
        v13[2] = ps2_vu_select(v13[2], 0.0f, 0);
        for (int i = 0; i < 4; i++) v13[i] = vp_lane_ftoi(v13[i], 4);
        x->vi[9] = (u16)(x->vi[9] - 1u);
        vp_sq(x, x->vi[8], v13, VP_MALL);
        x->vi[8] = (u16)(x->vi[8] + 1u);
        x->q = q;
    } while (x->vi[9] != 0u);
    return o;
}

static void intersect(vpx *x) {
    float *v2 = F(2), *v3 = F(3), *v26 = F(26), *v31 = F(31), q;
    int lane, add;
    switch (x->vi[14]) {
    case 0x040: lane = 1; add = 0; x->vi[1] = 0x100; x->vi[2] = 0x080; break;
    case 0x080: lane = 0; add = 0; x->vi[1] = 0x100; x->vi[2] = 0x200; break;
    case 0x100: lane = 2; add = 1; x->vi[1] = 0x400; x->vi[2] = 0x200; break;
    case 0x200: lane = 1; add = 1; x->vi[1] = 0x400; x->vi[2] = 0x200; break;
    case 0x400: lane = 0; add = 1; x->vi[1] = 0x400; x->vi[2] = 0x200; break;
    default:    lane = 0; add = 0; x->vi[1] = 0x400; x->vi[2] = 0x200; break;
    }
    v2[lane] = add ? fa(v31[lane], v31[3]) : fs(v31[lane], v31[3]);
    v3[lane] = add ? fa(v26[lane], v26[3]) : fs(v26[lane], v26[3]);
    for (int i = 0; i < 4; i++) v2[i] = fs(v2[i], v3[i]);
    for (int i = 0; i < 4; i++) v3[i] = vabs(v3[i]);
    for (int i = 0; i < 4; i++) v2[i] = vabs(v2[i]);
    q = ps2_fdiv(v3[lane], v2[lane]);
    x->q = q;
    for (int i = 0; i < 4; i++) F(13)[i] = fs(F(27)[i], F(22)[i]);
    for (int i = 0; i < 4; i++) F(16)[i] = fs(F(30)[i], F(25)[i]);
    for (int i = 0; i < 4; i++) F(13)[i] = fm(F(13)[i], q);
    for (int i = 0; i < 4; i++) F(16)[i] = fm(F(16)[i], q);
    for (int i = 0; i < 4; i++) F(13)[i] = fa(F(13)[i], F(22)[i]);
    for (int i = 0; i < 4; i++) F(16)[i] = fa(F(16)[i], F(25)[i]);
    rows4(F(1), x, 9, F(13));
    F(16)[3] = mfir(clip_flags(x, F(1)));
    vp_sq(x, x->vi[12] + 0u, F(13), VP_MALL);
    vp_sq(x, x->vi[12] + 3u, F(16), VP_MALL);
    vp_sq(x, x->vi[12] + 4u, F(1), VP_MALL);
    x->vi[12] = (u16)(x->vi[12] + 5u);
    x->vi[9] = (u16)(x->vi[9] + 1u);
}

static void edge_ends(vpx *x, u16 out, u16 in) {
    F(27)[3] = fm(1.0f, 1.0f);
    vp_lq(x, 22, out, VP_MXYZ);
    F(22)[3] = fm(1.0f, 1.0f);
    vp_lq(x, 25, out + 3u, VP_MX | VP_MY | VP_MW);
    F(25)[2] = fa(0.0f, 1.0f);
    vp_lq(x, 30, in + 3u, VP_MX | VP_MY | VP_MW);
    F(30)[2] = fa(0.0f, 1.0f);
    vp_lq(x, 27, in, VP_MXYZ);
    vp_lq(x, 26, out + 4u, VP_MALL);
    vp_lq(x, 31, in + 4u, VP_MALL);
    intersect(x);
}

static u32 clip_triangle(vpx *x, u32 o) {
    static const u8 src[9] = { 0, 3, 4, 5, 8, 9, 10, 13, 14 };
    u16 *vi = x->vi;
    st_tris_clipped++;
    for (int k = 0; k < 9; k++) {
        vp_lq(x, 1 + (k & 3), vi[10] + src[k], VP_MALL);
        vp_sq(x, 0x370u + src[k], F(1 + (k & 3)), VP_MALL);
    }
    vp_isw(x, 0x0Fu, 0, vi[10]);
    vp_isw(x, 0x0Fu, 1, vi[13]);
    vp_isw(x, 0x0Fu, 3, vi[10]);
    vi[12] = 0x39D;
    vi[13] = 3;
    vi[14] = 0x40;
    vi[10] = 0x370;
    for (;;) {
        u16 a;
        vi[9] = 0;
        vp_lq(x, 1, vi[10] + 0u, VP_MALL);
        vp_lq(x, 2, vi[10] + 3u, VP_MALL);
        vp_lq(x, 3, vi[10] + 4u, VP_MALL);
        vi[1] = (u16)(vi[13] * 5u + vi[10]);
        vp_sq(x, vi[1] + 0u, F(1), VP_MALL);
        vp_sq(x, vi[1] + 3u, F(2), VP_MALL);
        vp_sq(x, vi[1] + 4u, F(3), VP_MALL);
        for (;;) {
            vi[11] = (u16)(vi[10] + 5u);
            vi[1] = vp_ilw(x, vi[10] + 3u, 3);
            vi[2] = vp_ilw(x, vi[11] + 3u, 3);
            vi[1] &= vi[14];
            if ((s16)vi[1] > 0) {
                vi[1] = (u16)(vi[2] & vi[14]);
                if (!((s16)vi[1] > 0)) edge_ends(x, vi[10], vi[11]);
            } else {
                vp_lq(x, 22, vi[10] + 0u, VP_MALL);
                vp_lq(x, 25, vi[10] + 3u, VP_MALL);
                vp_lq(x, 26, vi[10] + 4u, VP_MALL);
                vp_sq(x, vi[12] + 0u, F(22), VP_MALL);
                vp_sq(x, vi[12] + 3u, F(25), VP_MALL);
                vp_sq(x, vi[12] + 4u, F(26), VP_MALL);
                vi[12] = (u16)(vi[12] + 5u);
                vi[9] = (u16)(vi[9] + 1u);
                vi[1] = (u16)(vi[2] & vi[14]);
                if ((s16)vi[1] > 0) edge_ends(x, vi[11], vi[10]);
            }
            vi[13] = (u16)(vi[13] - 1u);
            if ((s16)vi[13] < 0) return o;
            if (vi[13] == 0u) break;
            vi[10] = (u16)(vi[10] + 5u);
        }
        vi[14] = (u16)(vi[14] + vi[14]);
        a = (u16)(0xA80u & vi[14]);
        vi[4] = a;
        vi[13] = vi[9];
        if ((s16)a > 0) { vi[10] = 0x39D; vi[12] = 0x370; }
        else            { vi[10] = 0x370; vi[12] = 0x39D; }
        vi[4] = 0x800;
        if (vi[14] == vi[4]) break;
    }
    vi[1] = (u16)(vi[13] - 2u);
    if ((s16)vi[1] > 0) {
        vi[9] = vi[13];
        vi[14] = 1;
        st_fans++;
        o = put_vertices(x, o);
    }
    return o;
}

static u32 put_run(vpx *x, u32 o) {
    u16 *vi = x->vi;
    vi[9] = vi[13];
    vi[1] = (u16)(vi[13] - 2u);
    vi[2] = (u16)(vi[1] * 4u);
    vi[10] = (u16)(vi[10] - vi[2] - vi[1]);
    vi[14] = 0;
    vp_isw(x, 0x0Fu, 3, vi[10]);
    st_runs++;
    return put_vertices(x, o);
}

static void reload_clip_rows(vpx *x) {
    for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x30u + (u32)k, VP_MALL);
}

int rn_vp_flat(vpx *x, u32 pc, u32 *kick_at) {
    u16 *vi = x->vi, n, chg, mode;
    u32 top, o = 0;
    (void)kick_at;
    if (pc == 0x0000u) { x->tpc = 0x0010u; return 0; }
    if (pc != 0x0010u) return -1;
    top = x->top & VP_QW_MASK;
    n = vp_ilw(x, top, 0);
    chg = vp_ilw(x, 0x1Bu, 2);
    mode = (s16)chg > 0 ? vp_ilw(x, 0x1Bu, 0) : vi[5];
    if (n == 0u || n > 400u || (n < 3u && (mode & 0x20u))) { st_declined++; return -1; }
    st_batches++;
    vi[6] = n;
    vi[1] = (u16)(top + 1u);
    vi[2] = chg;
    vi[3] = (u16)(n + n);
    vi[7] = (u16)(vi[3] * 2u + n + vi[1]);
    vi[8] = vi[7];
    if ((s16)chg > 0) {
        vp_isw(x, 0x1Bu, 2, 0);
        vi[5] = vp_ilw(x, 0x1Bu, 0);
        for (int k = 0; k < 4; k++) vp_lq(x, 21 + k, 0x28u + (u32)k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x10u + (u32)k, VP_MALL);
        for (int k = 0; k < 4; k++) rows4(F(5 + k), x, 1, F(21 + k));
        for (int k = 0; k < 4; k++) vp_lq(x, 1 + k, 0x14u + (u32)k, VP_MALL);
        for (int k = 0; k < 4; k++) rows4(F(9 + k), x, 1, F(21 + k));
        for (int k = 0; k < 4; k++) vp_sq(x, 0x30u + (u32)k, F(9 + k), VP_MALL);
    } else {
        reload_clip_rows(x);
    }
    vi[1] = 0x20;
    vi[2] = (u16)(vi[5] & 0x20u);
    if (vi[2] == 0x20u) {
        vi[1] = (u16)top;
        vi[10] = (u16)(top + 1u);
        vi[4] = 0;
        vp_lq(x, 13, vi[10], VP_MXYZ);
        do {
            float p[4] = { F(13)[0], F(13)[1], F(13)[2], 1.0f };
            u16 fl;
            rows4(F(1), x, 9, p);
            vi[10] = (u16)(vi[10] + 5u);
            vp_sq(x, vi[10] - 1u, F(1), VP_MALL);
            vp_lq(x, 13, vi[10], VP_MXYZ);
            fl = clip_flags(x, F(1));
            vp_isw(x, vi[10] - 2u, 3, fl);
            vi[4] |= fl;
        } while (vi[10] != vi[7]);
        if (vi[4] != 0u) {
            vi[1] = (u16)top;
            vi[10] = (u16)(top + 1u);
            vi[2] = vp_ilw(x, vi[10] + 8u, 3);
            vi[1] = vp_ilw(x, vi[10] + 3u, 3);
            vi[13] = 0;
            if (vi[2] == 0u) {
                vi[13]++;
                if (vi[1] == 0u) vi[13]++;
            }
            for (;;) {
                vi[1] = vp_ilw(x, vi[10] + 0x0Du, 3);
                if (vi[1] == 0u) {
                    vi[13]++;
                } else {
                    vi[1] = (u16)(vi[13] - 3u);
                    if ((s16)vi[1] >= 0) {
                        vp_isw(x, 0x0Fu, 0, vi[10]);
                        o = put_run(x, o);
                        reload_clip_rows(x);
                        vi[10] = vp_ilw(x, 0x0Fu, 0);
                    }
                    vi[13] = 0;
                }
                vi[1] = (u16)(vi[13] - 3u);
                if ((s16)vi[1] < 0) {
                    u16 a = vp_ilw(x, vi[10] + 3u, 3), b = vp_ilw(x, vi[10] + 8u, 3),
                        c = vp_ilw(x, vi[10] + 0x0Du, 3);
                    vi[2] = b;
                    vi[3] = c;
                    vi[1] = (u16)(a & b & c);
                    if (vi[1] == 0u) {
                        o = clip_triangle(x, o);
                        vi[10] = vp_ilw(x, 0x0Fu, 0);
                        vi[13] = vp_ilw(x, 0x0Fu, 1);
                        reload_clip_rows(x);
                    } else {
                        st_rejected++;
                    }
                }
                vi[10] = (u16)(vi[10] + 5u);
                vi[1] = (u16)(vi[7] - 0x0Au);
                if (vi[10] == vi[1]) break;
            }
            vi[1] = (u16)(vi[13] - 3u);
            if ((s16)vi[1] >= 0) o = put_run(x, o);
            goto end;
        }
    }
    vi[9] = vi[6];
    vi[1] = (u16)top;
    vi[10] = (u16)(top + 1u);
    vp_isw(x, 0x0Fu, 3, vi[10]);
    vi[14] = 0;
    st_runs++;
    o = put_vertices(x, o);
end:
    vp_lq(x, 1, 0x02u, VP_MALL);
    vp_sq(x, vi[8], F(1), VP_MALL);
    for (u16 a = vi[7]; ; a = (u16)(a + 1u)) {
        float t[4];
        vp_rd(x, a, t);
        pkt_putf(o++, t);
        if (a == vi[8]) break;
    }
    pkt_kick(vi[7]);
    vp_isw(x, 0x3FFu, 0, vi[13]);
    vp_isw(x, 0x3FFu, 1, vi[9]);
    vp_isw(x, 0x3FFu, 2, vi[8]);
    vp_isw(x, 0x3FFu, 3, vi[7]);
    x->tpc = 0x0010u;
    return 1;
}

void rn_vp_flat_report(void) {
    if (st_batches | st_declined)
        ps2_log("rn: native flat strip program -- %llu batches: %llu runs, %llu triangles "
                "clipped (%llu fans), %llu rejected; %llu batches left to VU1",
                (unsigned long long)st_batches, (unsigned long long)st_runs,
                (unsigned long long)st_tris_clipped, (unsigned long long)st_fans,
                (unsigned long long)st_rejected, (unsigned long long)st_declined);
}
