#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_trees, st_culled, st_batches[3];

#define F(r) (x->vf[r].f)

static int mac_sign(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) && (u & 0x7FFFFFFFu);
}

static float vmax(float a, float b) { return ps2_vu_select(a, b, 0); }
static float vmin(float a, float b) { return ps2_vu_select(a, b, 1); }

static int tree(vpx *x, u32 rec, u16 t, int mode1, u32 o) {
    float *v1 = F(1), *v2 = F(2), *v3 = F(3), *v13 = F(13), *v14 = F(14), *v15 = F(15);
    float *v18 = F(18), *v19 = F(19), *v21 = F(21), *v22 = F(22), *v24 = F(24);
    float *v25 = F(25), *v26 = F(26), *v27 = F(27), *v28 = F(28), *v29 = F(29);
    float tag_a[4], test[4], tag_s[4], tex[4], q, p, acc;
    int far_path;

    vp_lq(x, 13, rec, VP_MALL);
    for (int i = 0; i < 3; i++) v13[i] = F(17)[i] + v13[i];
    v18[0] = 0.0f; v18[1] = 0.0f;
    for (int i = 0; i < 3; i++) v19[i] = 1.0f;
    vp_lq(x, 1, (u32)(t + 0x27u), VP_MALL);
    v18[2] = 1.0f;
    vp_lq(x, 3, (u32)(t + 0x27u), VP_MALL);
    vp_rows(v1, F(9), F(10), F(11), F(12), v13[0], v13[1], v13[2], v13[3], VP_MALL);
    v18[0] = 0.0f + v3[0];
    v19[0] = 0.0f + v3[1];
    for (int i = 0; i < 3; i++) v25[i] = v13[i] - F(20)[i];
    v3[2] = v1[2] + v1[3];
    acc = 0.0f + v1[3];
    v2[2] = acc - v1[2] * 1.0f;
    p = sqrtf(v25[0] * v25[0] + v25[1] * v25[1] + v25[2] * v25[2]);
    if (mac_sign(v3[2]) || mac_sign(v2[2])) return 0;

    v1[0] = F(16)[0] - F(16)[1];
    for (int i = 0; i < 4; i++) v22[i] = p;
    v1[1] = v22[1] - F(16)[1];
    q = ps2_fdiv(v1[1], v1[0]);
    v1[0] = 0.0f + q; v1[1] = 0.0f + q; v1[2] = 0.0f + q; v1[3] = 1.0f + q;
    v2[0] = v1[1]; v2[1] = v1[2]; v2[2] = v1[3]; v2[3] = v1[0];
    {
        float acc[4] = { 0.0f + 127.0f, 0.0f + 127.0f, 0.0f + 127.0f, 1.0f + 127.0f };
        for (int i = 0; i < 4; i++) v21[i] = acc[i] - v2[i] * 127.0f;
    }
    far_path = mac_sign(v21[3] - 127.0f);
    vp_rd(x, 0x02u, tag_a);
    if (!far_path) {
        vp_rd(x, 0x2Du, test);
        vp_rd(x, mode1 ? 0x01u : 0x06u, tag_s);
        {   float i0 = mode1 ? 128.0f : 127.0f;
            v21[0] = 0.0f + i0; v21[1] = 0.0f + i0; v21[2] = 0.0f + i0; v21[3] = 1.0f + i0; }
    } else {
        vp_rd(x, 0x2Eu, test);
        vp_rd(x, mode1 ? 0x00u : 0x05u, tag_s);
    }
    memcpy(F(23), tag_s, 16);
    memcpy(v1, tag_a, 16);
    memcpy(v2, test, 16);

    for (int i = 0; i < 4; i++) v2[i] = F(16)[i] - v22[i];
    if (mac_sign(v2[0])) return 0;
    if (mac_sign(v2[2]))      vp_rd(x, (u32)(t + 0x21u), tex);
    else if (mac_sign(v2[3])) vp_rd(x, (u32)(t + 0x1Eu), tex);
    else                      vp_rd(x, (u32)(t + 0x1Bu), tex);
    memcpy(v2, tex, 16);

    vp_rows(v13, F(5), F(6), F(7), F(8), v13[0], v13[1], v13[2], v13[3], VP_MALL);
    q = ps2_fdiv(1.0f, v13[3]);
    v13[2] = v13[2] + F(4)[3];
    for (int i = 0; i < 3; i++) v13[i] = v13[i] * q;
    vp_lq(x, 1, 0x32u, VP_MALL);
    acc = 1.0f * v1[0];
    v13[3] = acc + v13[3] * v1[1];
    v13[3] = vmin(v13[3], 255.0f);
    v13[2] = vmin(v13[2], 16777215.0f);
    v13[3] = vmax(v13[3], 0.0f);
    v13[2] = vmax(v13[2], 0.0f);
    vp_lq(x, 1, (u32)(t + 0x24u), VP_MALL);
    v1[0] = v1[0] * F(4)[2]; v1[1] = v1[1] * F(4)[2];
    v1[0] = v1[0] * q;       v1[1] = v1[1] * q;
    v13[0] = v13[0] + v1[0]; v13[1] = v13[1] + v1[1];
    vp_lq(x, 2, (u32)(t + 0x2Au), VP_MALL);
    v1[0] = 0.0f + v2[0];    v1[1] = 0.0f + v2[1];
    v1[0] = v1[0] * F(4)[2]; v1[1] = v1[1] * F(4)[2];
    v1[0] = v1[0] * q;       v1[1] = v1[1] * q;
    v2[0] = v1[0] * v1[0];   v2[1] = v1[1] * v1[1];
    v3[0] = v13[0] - 2048.0f; v3[1] = v13[1] - 2048.0f;
    v2[0] = v2[0] + v2[1];
    v3[0] = v3[0] * v3[0];   v3[1] = v3[1] * v3[1];
    v2[0] = v2[0] + 462848.0f;
    v3[0] = v3[0] + v3[1];
    if (mac_sign(v2[0] - v3[0])) return 0;

    v14[0] = v14[1] = v14[2] = 0.0f; v14[3] = 1.0f;
    v15[0] = v15[1] = v15[2] = 0.0f; v15[3] = 1.0f;
    v14[0] = v13[0] - v1[0]; v14[1] = v13[1] - v1[1];
    v15[0] = v13[0] + v1[0]; v15[1] = v13[1] + v1[1];
    v14[2] = v13[2] + 0.0f;  v15[2] = v13[2] + 0.0f;
    v14[3] = v13[3] * 1.0f;  v15[3] = v13[3] * 1.0f;
    v1[0] = 0.0f + 2304.0f;  v1[1] = 0.0f + 2272.0f;
    v2[0] = 0.0f + 1792.0f;  v2[1] = 0.0f + 1824.0f;
    if (!mac_sign(v14[0] - v1[0]) || !mac_sign(v14[1] - v1[1])) return 0;
    if (mac_sign(v15[0] - v2[0]) || mac_sign(v15[1] - v2[1])) return 0;

    v25[0] = v15[0] - v14[0]; v25[1] = v15[1] - v14[1];
    v26[0] = v1[0] - v14[0];  v26[1] = v1[1] - v14[1];
    v27[0] = v2[0] - v14[0];  v27[1] = v2[1] - v14[1];
    v28[0] = v19[0] - v18[0]; v28[1] = v19[1] - v18[1];
    v29[0] = 0.0f + v18[0];   v29[1] = 0.0f + v18[1];
    if (mac_sign(v14[0] - v2[0])) {
        q = ps2_fdiv(v27[0], v25[0]);
        v14[0] = 0.0f + v2[0];
        v3[0] = v28[0] * q;
        v18[0] = v29[0] + v3[0];
    }
    if (mac_sign(v14[1] - v2[1])) {
        q = ps2_fdiv(v27[1], v25[1]);
        v18[1] = 0.0f + q;
        v14[1] = 0.0f + v2[1];
    }
    if (!mac_sign(v15[0] - v1[0])) {
        q = ps2_fdiv(v26[0], v25[0]);
        v15[0] = 0.0f + v1[0];
        v3[0] = v28[0] * q;
        v19[0] = v29[0] + v3[0];
    }
    if (!mac_sign(v15[1] - v1[1])) {
        q = ps2_fdiv(v26[1], v25[1]);
        v19[1] = 0.0f + q;
        v15[1] = 0.0f + v1[1];
    }

    v24[0] = v24[1] = v24[2] = 0.0f + 128.0f;
    v21[0] = v21[0] - 0.0f; v21[1] = v21[1] - 0.0f;
    v21[2] = v21[2] - 0.0f; v21[3] = v21[3] - 1.0f;
    v24[3] = v21[3] + 1.0f;
    for (int i = 0; i < 4; i++) {
        s32 c = vp_ftoi(v24[i], 0);
        memcpy(&v24[i], &c, 4);
    }

    pkt_putf(o + 0u, tag_a);
    pkt_putf(o + 1u, test);
    pkt_putf(o + 2u, tex);
    pkt_putf(o + 3u, tag_s);
    pkt_putf(o + 4u, v24);
    pkt_putf(o + 5u, v18);
    pkt_put_xyzf(o + 6u, v14, vp_ftoi(v14[3], 4));
    pkt_putf(o + 7u, v24);
    pkt_putf(o + 8u, v19);
    pkt_put_xyzf(o + 9u, v15, vp_ftoi(v15[3], 4));
    for (int i = 0; i < 4; i++) {
        s32 a = vp_ftoi(v14[i], 4), b = vp_ftoi(v15[i], 4);
        memcpy(&v14[i], &a, 4);
        memcpy(&v15[i], &b, 4);
    }
    return 1;
}

int rn_vp_trees(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, o = 0;
    u16 mode, count;
    if (pc == 0x0000u) {
        for (int k = 0; k < 8; k++) vp_lq(x, 5 + k, 0x10u + (u32)k, VP_MALL);
        vp_lq(x, 4, 0x18u, VP_MALL);
        vp_lq(x, 16, 0x19u, VP_MALL);
        vp_lq(x, 20, 0x1Au, VP_MALL);
        x->tpc = 0x0068u;
        return 0;
    }
    if (pc != 0x0068u) return -1;
    top = x->top & VP_QW_MASK;
    mode = vp_ilw(x, top, 1);
    count = vp_ilw(x, top, 0);
    if (!count) return -1;
    x->vi[15] = (u16)top;
    x->vi[1] = mode;
    x->vi[2] = 1;
    x->vi[3] = 2;
    if (mode != 1u) {
        u16 t = vp_ilw(x, top, 3);
        u16 out = (u16)(top + vp_ilw(x, top, 2));
        x->vi[6] = count;
        x->vi[11] = t;
        vp_lq(x, 17, top + 1u, VP_MALL);
        x->vi[14] = (u16)(top + 2u);
        x->vi[10] = 0;
        x->vi[12] = out;
        x->vi[13] = vp_ilw(x, top, 2);
        st_batches[mode == 0u ? 0 : 2]++;
        do {
            if (tree(x, x->vi[14], t, 0, o)) {
                pkt_kick(x->vi[12]);
                o += 10u;
                x->vi[12] = (u16)(x->vi[12] + 10u);
                st_trees++;
            } else {
                st_culled++;
            }
            x->vi[10]++;
            x->vi[14]++;
        } while (x->vi[6] != x->vi[10]);
        x->tpc = 0x0068u;
        return o ? 1 : 0;
    }

    {
        u16 out = (u16)(top + vp_ilw(x, top, 2));
        x->vi[13] = out;
        x->vi[12] = out;
        x->vi[14] = (u16)(top + 1u);
        x->vi[6] = 0;
        st_batches[1]++;
        for (;;) {
            u16 ends[3];
            vp_lq(x, 17, x->vi[14], VP_MALL);
            ends[0] = vp_ilw(x, x->vi[14] + 1u, 0);
            ends[1] = vp_ilw(x, x->vi[14] + 1u, 1);
            ends[2] = vp_ilw(x, x->vi[14] + 1u, 2);
            x->vi[7] = ends[0]; x->vi[9] = ends[1]; x->vi[8] = ends[2];
            x->vi[14] = (u16)(x->vi[14] + 2u);
            x->vi[11] = 0;
            x->vi[10] = 0;
            while (x->vi[11] != 3u) {
                if (x->vi[10] == ends[x->vi[11]]) { x->vi[11]++; continue; }
                if (tree(x, x->vi[14], x->vi[11], 1, o)) {
                    o += 10u;
                    x->vi[12] = (u16)(x->vi[12] + 10u);
                    st_trees++;
                } else {
                    st_culled++;
                }
                x->vi[14]++;
                x->vi[10]++;
                if (o > 10u * 1024u) return o ? 1 : 0;
            }
            x->vi[6]++;
            if (x->vi[6] == count) break;
        }
        {
            float term[4];
            vp_rd(x, 0x04u, term);
            pkt_putf(o, term);
        }
        x->vi[1] = 3;
        *kick_at = out;
        x->tpc = 0x0068u;
        return 1;
    }
}

void rn_vp_sprite_report(void) {
    if (st_trees || st_culled)
        ps2_log("rn: native tree billboards -- %llu sprites, %llu culled; batches mode 0 "
                "%llu, grouped %llu, mode 2 %llu", (unsigned long long)st_trees,
                (unsigned long long)st_culled, (unsigned long long)st_batches[0],
                (unsigned long long)st_batches[1], (unsigned long long)st_batches[2]);
}
