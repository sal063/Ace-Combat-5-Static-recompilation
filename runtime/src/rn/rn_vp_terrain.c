#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>

static u64 st_patches, st_outside, st_declined, st_room, st_full;

static float height(vpx *x, u32 a) {
    return vp_lane_itof(vp_rd_lane(x, a, 1), 0) * 80.0f;
}

static float clip_w(vpx *x, const float *p) {
    float acc = vp_F(x, 9)[3] * p[0];
    acc = acc + vp_F(x, 10)[3] * p[1];
    acc = acc + vp_F(x, 11)[3] * p[2];
    return acc + vp_F(x, 12)[3] * 1.0f;
}

static void build_patch(vpx *x, u32 t, u16 count) {
    u32 v = t + 3u;
    float org[4], uvoff[4];
    vp_rd(x, t + 1u, org);
    vp_rd(x, t + 2u, uvoff);
    if (count == 10u) {
        float pos[9][4], uv[9][4];
        for (int k = 0; k < 9; k++) {
            float tmpl[4];
            vp_rd(x, 0x3ECu + k, tmpl);
            pos[k][0] = tmpl[0] + org[0];
            pos[k][1] = height(x, v + 4u * k);
            pos[k][2] = tmpl[2] + org[2];
            pos[k][3] = tmpl[3];
            vp_rd(x, 0x3F6u + k, uv[k]);
            uv[k][2] = uvoff[2] + 0.0f;
            uv[k][3] = uvoff[3] + 0.0f;
        }
        for (int k = 0; k < 9; k++) {
            vp_sq(x, v + 4u * k + 3u, uv[k], VP_MALL);
            if (k & 1) vp_sq(x, v + 4u * k, pos[k], VP_MALL);
        }
        vp_sq(x, v + 36u, pos[1], VP_MALL);
        vp_sq(x, v + 39u, uv[1], VP_MALL);
        for (int k = 0; k < 9; k += 2) {
            pos[k][3] = clip_w(x, pos[k]);
            vp_sq(x, v + 4u * k, pos[k], VP_MALL);
        }
        {
            float g[4], range;
            static const u8 nb[5][2] = { {1, 5}, {3, 1}, {5, 3}, {7, 5}, {9, 7} };
            vp_rd(x, 0x21u, g);
            range = g[1] - g[0];
            for (int e = 0; e < 5; e++) {
                u32 a = v + 4u * (u32)(e * 2);
                float p[4], d, avg, h, q;
                vp_rd(x, a, p);
                d = p[3] - g[0];
                if (signbit(d)) continue;
                h = p[1];
                avg = vp_rd_lane(x, v + 4u * nb[e][1], 1) + vp_rd_lane(x, v + 4u * nb[e][0], 1);
                avg = avg * 0.5f;
                q = d / range;
                if (!signbit(p[3] - g[1])) {
                    p[1] = avg;
                } else {
                    float s = avg - h;
                    s = s * q;
                    p[1] = h + s;
                }
                vp_sq(x, a, p, VP_MY);
            }
        }
    } else {
        for (int k = 0; k < 4; k++) {
            float pos[4], uv[4], tmpl[4];
            pos[1] = height(x, v + 4u * k);
            vp_rd(x, 0x3F7u + 2u * k, uv);
            uv[2] = uvoff[2] + 0.0f;
            uv[3] = uvoff[3] + 0.0f;
            vp_rd(x, 0x3EDu + 2u * k, tmpl);
            pos[0] = tmpl[0] + org[0];
            pos[2] = tmpl[2] + org[2];
            pos[3] = tmpl[3];
            vp_sq(x, v + 4u * k + 3u, uv, VP_MALL);
            vp_sq(x, v + 4u * k, pos, VP_MALL);
        }
    }
}

static u16 clip_code(vpx *x, const float *p) {
    float c[4];
    vp_rows(c, vp_F(x, 9), vp_F(x, 10), vp_F(x, 11), vp_F(x, 12), p[0], p[1], p[2], 1.0f,
            VP_MALL);
    return vp_clip_code(c);
}

int rn_vp_terrain(vpx *x, u32 pc, u32 *kick_at) {
    static u8 recs[10u * 64u];
    static u32 idx[10u * 3u];
    static const u32 tmpl[4] = { 0x00u, 0x01u, 0x02u, 0x03u };
    static const u32 data[4][2] = { {0x19u, 0x23u}, {0x1Au, 0x1Eu}, {0x1Cu, 0x1Fu},
                                    {0x1Du, 0x20u} };
    u32 t, v, prims[4], ni, vbase = 0, ifirst = 0, passes = 0;
    u16 count, flags;
    int stored;
    ps2_vk_mesh_block b;
    float fogc[4];
    (void)kick_at;
    if (pc == 0x0000u) {
        for (int k = 0; k < 4; k++) vp_lq(x, 5 + k, 0x10u + k, VP_MALL);
        for (int k = 0; k < 4; k++) vp_lq(x, 9 + k, 0x14u + k, VP_MALL);
        x->tpc = 0x0050u;
        return 0;
    }
    if (pc != 0x0050u || !x->native || !vp_mesh_wanted()) { st_declined++; return -1; }
    t = x->top & VP_QW_MASK;
    count = vp_ilw(x, t, 0);
    flags = vp_ilw(x, t, 1);
    if (count != 4u && count != 10u) { st_declined++; return -1; }
    for (int pass = 0; pass < 4; pass++) {
        prims[pass] = 0;
        if (pass && !((flags >> (pass - 1)) & 1u)) continue;
        if (!vp_tag_prim(x, tmpl[pass], &prims[pass]) || !vp_prim_tris(prims[pass])) {
            st_declined++;
            return -1;
        }
        passes++;
    }
    if (!ps2_vk_mesh_room(count, vp_tri_index_bound(count), passes)) {
        st_room++;
        st_declined++;
        return -1;
    }

    x->tpc = 0x0050u;
    build_patch(x, t, count);
    v = t + 3u;
    if (flags & 0x10u) {
        u16 all = 0xFFF;
        for (u32 k = 0; k < count; k++) {
            float p[4];
            vp_rd(x, v + 4u * k, p);
            all &= clip_code(x, p);
        }
        if (all) { st_outside++; return 0; }
    }

    vp_copy_records(x, v, 4u, count, recs);
    ni = vp_tri_indices(prims[0] & 7u, count, 0, 0u, 0u, idx);
    stored = ni && ps2_vk_mesh_store(recs, count, idx, ni, &vbase, &ifirst) == 0;
    if (ni && !stored) st_full++;

    rn_mesh_block_init(&b);
    for (int k = 0; k < 4; k++) memcpy(b.m + 4 * k, vp_F(x, 5 + k), 16);
    for (int k = 0; k < 4; k++) memcpy(b.c + 4 * k, vp_F(x, 9 + k), 16);
    vp_rd(x, 0x18u, fogc);
    b.fog0 = fogc[0];
    b.fog1 = fogc[1];
    b.clip = (flags & 0x10u) ? 1u : 0u;
    b.prog = 1u;
    vp_rd(x, 0x21u, b.p + 0);
    vp_rd(x, 0x22u, b.p + 4);
    vp_rd(x, 0x0Du, b.p + 8);
    b.p[12] = (flags & 6u) ? 128.0f : 255.0f;

    for (int pass = 0; pass < 4; pass++) {
        ps2_vk_state st;
        u32 w[4];
        if (pass && !((flags >> (pass - 1)) & 1u)) continue;
        memcpy(w, vp_q(x, 0x06u), 16);
        pkt_put(0, w);
        memcpy(w, vp_q(x, data[pass][0]), 16);
        pkt_put(1, w);
        memcpy(w, vp_q(x, data[pass][1]), 16);
        pkt_put(2, w);
        pkt_flush();
        ps2_gs_write_reg(0x00, prims[pass]);
        if (!stored) continue;
        ps2_gs_native_frame(&b.xoff, &b.yoff, &b.zmax);
        ps2_gs_native_state(&st);
        b.pass = (u32)pass;
        ps2_vk_draw_mesh(&st, &b, vbase, ifirst, ni, 0);
    }
    st_patches++;
    return 0;
}

void rn_vp_terrain_report(void) {
    if (st_patches || st_declined)
        ps2_log("rn: native terrain -- %llu patches drawn by mesh.vert (%llu with "
                "a clip test finding them wholly outside, %llu not stored), %llu "
                "left to VU1 (%llu of them for mesh arena space)",
                (unsigned long long)st_patches, (unsigned long long)st_outside,
                (unsigned long long)st_full, (unsigned long long)st_declined,
                (unsigned long long)st_room);
}
