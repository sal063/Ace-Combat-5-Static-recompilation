#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_billboard_on;

static struct {
    int active;
    int drawn;
    rn_int_billboard b;
} bb;

static struct { u64 sprites, native, fallback, claimed; } st_bb;

void rn_billboard_init(void) {
    const char *e = getenv("PS2_RN_BILLBOARD");
    rn_billboard_on = !(e && *e == '0');
}

void rn_billboard_intent(const u8 *rec, u32 len) {
    if (!rn_billboard_on || len < sizeof(rn_intent_hdr) + sizeof bb.b) return;
    memcpy(&bb.b, rec + sizeof(rn_intent_hdr), sizeof bb.b);
    bb.active = 1;
    bb.drawn = 0;
    st_bb.sprites++;
}

void rn_billboard_end(void) { bb.active = 0; }
int rn_billboard_active(void) { return bb.active; }

static int draw_billboard(const ps2_vk_state *claimed) {
    static const s32 corner[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    u8 recs[4 * 64];
    u32 idx[6] = { 0, 1, 2, 1, 3, 2 };
    u32 vbase, ifirst;
    ps2_vk_mesh_block blk;
    ps2_vk_state st = *claimed;
    float u0 = bb.b.uv[0] / 16.0f, v0 = bb.b.uv[1] / 16.0f;
    float u1 = bb.b.uv[2] / 16.0f, v1 = bb.b.uv[3] / 16.0f;
    s32 col[4];
    if (!ps2_vk_mesh_available()) return 0;
    rn_rgba_lanes(bb.b.rgba, col);
    for (u32 i = 0; i < 4u; i++) {
        s32 nrm[4] = { corner[i][0], corner[i][1], 0, 0 };
        float uv[4] = { corner[i][0] < 0 ? u0 : u1, corner[i][1] < 0 ? v0 : v1,
                        bb.b.size, 0.0f };
        rn_mesh_rec(recs + 64u * i, bb.b.pos, nrm, col, uv);
    }
    if (ps2_vk_mesh_store(recs, 4u, idx, 6u, &vbase, &ifirst) != 0) return 0;
    rn_mesh_block_init(&blk);
    memcpy(blk.m, bb.b.rows, sizeof blk.m);
    blk.p[0] = bb.b.scale[0];
    blk.p[1] = bb.b.scale[1];
    blk.p[2] = (float)bb.b.zmin;
    blk.fog0 = 255.0f;
    blk.prog = 3u;
    ps2_vk_draw_mesh(&st, &blk, vbase, ifirst, 6u, 0);
    return 1;
}

int rn_billboard_claim_prim(const struct ps2_vk_state *st, int kind,
                            const struct ps2_vk_vertex *v, int n) {
    (void)kind; (void)v; (void)n;
    if (!bb.drawn) {
        bb.drawn = draw_billboard(st) ? 1 : -1;
        if (bb.drawn > 0) st_bb.native++;
        else st_bb.fallback++;
    }
    if (bb.drawn < 0) return 0;
    st_bb.claimed++;
    return 1;
}

void rn_billboard_report(void) {
    if (!st_bb.sprites) return;
    ps2_log("rn: native billboards -- %llu cloud sprites: %llu drawn by mesh.vert (%llu "
            "sprite pieces claimed), %llu left emulated",
            (unsigned long long)st_bb.sprites, (unsigned long long)st_bb.native,
            (unsigned long long)st_bb.claimed, (unsigned long long)st_bb.fallback);
}
