#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_tri3d_on;

static struct {
    int active;
    int drawn;
    rn_int_tri3d t;
} tri;

static struct { u64 tris, native, fallback, claimed; } st_tri;

void rn_tri3d_init(void) {
    const char *e = getenv("PS2_RN_TRI3D");
    rn_tri3d_on = !(e && *e == '0');
}

void rn_tri3d_intent(const u8 *rec, u32 len) {
    if (!rn_tri3d_on || len < sizeof(rn_intent_hdr) + sizeof tri.t) return;
    memcpy(&tri.t, rec + sizeof(rn_intent_hdr), sizeof tri.t);
    tri.active = 1;
    tri.drawn = 0;
    st_tri.tris++;
}

void rn_tri3d_end(void) {
    tri.active = 0;
}

int rn_tri3d_active(void) {
    return tri.active;
}

static s32 channel(float c) {
    if (c >= 2147483600.0f) return (s32)(ps2_cvt_w_s(c - 2147483600.0f) | 0x80000000);
    return ps2_cvt_w_s(c);
}

static int draw_tri(const ps2_vk_state *claimed) {
    u8 recs[3 * 64];
    u32 idx[3] = { 0, 1, 2 };
    u32 vbase, ifirst;
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    (void)claimed;
    if (!ps2_vk_mesh_available()) return 0;
    for (u32 i = 0; i < 3u; i++) {
        const float *c = tri.t.colour + 4u * i, *uv = tri.t.uv + 4u * i;
        const float *p = tri.t.pos + 4u * i;
        float pos[4] = { p[0], p[1], p[2], 1.0f };
        float stq[4] = { uv[0], uv[1], uv[2] != 0.0f ? uv[2] : 1.0f, 0.0f };
        s32 col[4] = { channel(c[0]), channel(c[1]), channel(c[2]), channel(c[3]) };
        rn_mesh_rec(recs + 64u * i, pos, NULL, col, stq);
    }
    if (ps2_vk_mesh_store(recs, 3u, idx, 3u, &vbase, &ifirst) != 0) return 0;
    rn_mesh_block_init(&b);
    memcpy(b.m, tri.t.screen, sizeof b.m);
    memcpy(b.c, tri.t.clip, sizeof b.c);
    b.fog0 = 255.0f;
    b.clip = 1;
    b.prog = 4u;
    ps2_gs_native_state(&st);
    ps2_vk_draw_mesh(&st, &b, vbase, ifirst, 3u, 0);
    return 1;
}

int rn_tri3d_claim_prim(const struct ps2_vk_state *st, int kind,
                        const struct ps2_vk_vertex *v, int n) {
    (void)kind; (void)v; (void)n;
    if (!tri.drawn) {
        tri.drawn = draw_tri(st) ? 1 : -1;
        if (tri.drawn > 0) st_tri.native++;
        else st_tri.fallback++;
    }
    if (tri.drawn < 0) return 0;
    st_tri.claimed++;
    return 1;
}

void rn_tri3d_report(void) {
    if (!st_tri.tris) return;
    ps2_log("rn: native clipped triangles -- %llu: %llu drawn by mesh.vert (%llu fan "
            "triangles claimed), %llu left emulated",
            (unsigned long long)st_tri.tris, (unsigned long long)st_tri.native,
            (unsigned long long)st_tri.claimed, (unsigned long long)st_tri.fallback);
}
