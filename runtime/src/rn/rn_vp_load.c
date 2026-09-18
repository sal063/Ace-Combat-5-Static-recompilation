#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_blocks[5];

#define F(r) (x->vf[r].f)

static void project(vpx *x, int r, float pre[4]) {
    float *v = F(r), q;
    vp_rows(v, F(5), F(6), F(7), F(8), v[0], v[1], v[2], v[3], VP_MALL);
    q = ps2_fdiv(1.0f, v[3]);
    for (int i = 0; i < 3; i++) v[i] = v[i] * q;
    memcpy(pre, v, 16);
    for (int i = 0; i < 4; i++) {
        s32 k = vp_ftoi(v[i], 4);
        memcpy(&v[i], &k, 4);
    }
}

int rn_vp_loadmap(vpx *x, u32 pc, u32 *kick_at) {
    u32 top, o = 0;
    u16 mode;
    s16 m80;
    float pos[21][4];
    if (pc == 0x0000u) {
        for (int k = 0; k < 8; k++) vp_lq(x, 5 + k, 0x10u + (u32)k, VP_MALL);
        vp_lq(x, 22, 0x06u, VP_MALL);
        vp_lq(x, 23, 0x07u, VP_MALL);
        vp_lq(x, 24, 0x08u, VP_MALL);
        x->tpc = 0x0068u;
        return 0;
    }
    if (pc != 0x0068u) return -1;
    top = x->top & VP_QW_MASK;

    vp_lq(x, 21, top, VP_MX | VP_MZ);
    vp_lq(x, 13, top + 1u, VP_MY);
    vp_lq(x, 17, top + 2u, VP_MY);
    vp_lq(x, 18, top + 2u, VP_MY);
    F(13)[3] = 1.0f * 1.0f;
    vp_lq(x, 19, top + 3u, VP_MY);
    F(13)[0] = F(21)[0] + 0.0f;
    F(13)[2] = F(21)[2] + 0.0f;
    vp_lq(x, 20, top + 3u, VP_MY);
    for (int k = 0; k < 5; k++) vp_lq(x, 25 + k, top + 4u + (u32)k, VP_MALL);
    for (int i = 0; i < 4; i++) {
        F(14)[i] = F(13)[i] + F(23)[i];
        F(15)[i] = F(13)[i] + F(22)[i];
        F(16)[i] = F(13)[i] + F(24)[i];
    }
    F(17)[0] = F(14)[0] + 0.0f; F(17)[2] = F(14)[2] + 0.0f; F(17)[3] = F(14)[3] + 0.0f;
    F(18)[0] = F(15)[0] + 0.0f; F(18)[2] = F(15)[2] + 0.0f; F(18)[3] = F(15)[3] + 0.0f;
    F(19)[0] = F(15)[0] + 0.0f; F(19)[2] = F(15)[2] + 0.0f; F(19)[3] = F(15)[3] + 0.0f;
    F(20)[0] = F(16)[0] + 0.0f; F(20)[2] = F(16)[2] + 0.0f; F(20)[3] = F(16)[3] + 0.0f;
    for (int r = 13; r <= 20; r++) project(x, r, pos[r]);
    x->vi[5] = (u16)top;
    x->vi[7] = (u16)(top + 9u);

#define PUT_TAG(a)  do { float t_[4]; vp_rd(x, (a), t_); pkt_putf(o++, t_); } while (0)
#define PUT_CLR(r)  pkt_putf(o++, F(r))
#define PUT_XYZ(r)  do { s32 w_; memcpy(&w_, &F(r)[3], 4); \
                         pkt_put_xyzf(o++, pos[r], w_); } while (0)

    mode = vp_ilw(x, top, 1);
    m80 = (s16)(u16)(mode - 0x80u);
    x->vi[6] = mode;
    if (m80 >= 0) {
        memcpy(F(30), F(25), 16);
        x->vi[6] = (u16)m80;
        PUT_TAG(0x04u); PUT_CLR(30);
        PUT_XYZ(13); PUT_XYZ(14); PUT_XYZ(15); PUT_XYZ(16); PUT_XYZ(13);
        if (!((u16)m80 & 1u)) {
            PUT_TAG(0x05u); PUT_CLR(30);
            PUT_XYZ(14); PUT_XYZ(17); PUT_XYZ(15); PUT_XYZ(18);
        }
        if (!((u16)m80 & 2u)) {
            PUT_TAG(0x05u); PUT_CLR(30);
            PUT_XYZ(16); PUT_XYZ(20); PUT_XYZ(15); PUT_XYZ(19);
        }
        st_blocks[4]++;
    } else {
        PUT_TAG(0x02u); PUT_XYZ(15); PUT_XYZ(14); PUT_CLR(25); PUT_XYZ(13); PUT_XYZ(16);
        if (mode == 0u || mode == 2u) {
            PUT_TAG(0x03u); PUT_CLR(28); PUT_XYZ(15); PUT_XYZ(14);
            PUT_CLR(26); PUT_XYZ(17); PUT_XYZ(18);
        }
        if (mode == 0u || mode == 1u) {
            PUT_TAG(0x03u); PUT_CLR(29); PUT_XYZ(15); PUT_XYZ(16);
            PUT_CLR(27); PUT_XYZ(20); PUT_XYZ(19);
        }
        st_blocks[mode < 3u ? mode : 3u]++;
    }
    PUT_TAG(0x0Au);
#undef PUT_TAG
#undef PUT_CLR
#undef PUT_XYZ
    x->vi[8] = (u16)(top + 9u + o);
    *kick_at = top + 9u;
    x->tpc = 0x0068u;
    return 1;
}

void rn_vp_load_report(void) {
    if (st_blocks[0] | st_blocks[1] | st_blocks[2] | st_blocks[3] | st_blocks[4])
        ps2_log("rn: native load-screen map -- blocks with both sides %llu, one "
                "side %llu / %llu, top only %llu, outlines %llu",
                (unsigned long long)st_blocks[0], (unsigned long long)st_blocks[1],
                (unsigned long long)st_blocks[2], (unsigned long long)st_blocks[3],
                (unsigned long long)st_blocks[4]);
}
