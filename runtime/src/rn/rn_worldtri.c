#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_worldtri_on;

#define WT_MAX 4096u

static struct {
    int active;
    int drawn;
    rn_int_worldtri h;
    rn_world_vtx v[WT_MAX];
} wt;

static struct { u64 batches, native, fallback, claimed, tris; } st_wt;

void rn_worldtri_init(void) {
    const char *e = getenv("PS2_RN_WORLDTRI");
    rn_worldtri_on = !(e && *e == '0');
}

void rn_worldtri_intent(const u8 *rec, u32 len) {
    const u32 base = sizeof(rn_intent_hdr) + sizeof wt.h;
    if (!rn_worldtri_on || len < base) return;
    memcpy(&wt.h, rec + sizeof(rn_intent_hdr), sizeof wt.h);
    if (!wt.h.n || wt.h.n > WT_MAX || wt.h.n % 3u || len < base + wt.h.n * sizeof wt.v[0])
        return;
    memcpy(wt.v, rec + base, wt.h.n * sizeof wt.v[0]);
    wt.active = 1;
    wt.drawn = 0;
    st_wt.batches++;
}

void rn_worldtri_end(void) { wt.active = 0; }
int rn_worldtri_active(void) { return wt.active; }

static int draw_worldtri(void) {
    static u8 recs[WT_MAX * 64u];
    static u32 idx[WT_MAX];
    u32 vbase, ifirst;
    ps2_vk_mesh_block b;
    ps2_vk_state st;
    if (!ps2_vk_mesh_available()) return 0;
    for (u32 i = 0; i < wt.h.n; i++) {
        const rn_world_vtx *v = &wt.v[i];
        float pos[4] = { v->pos[0], v->pos[1], v->pos[2], 1.0f };
        float uv[4] = { v->uv[0], v->uv[1], 0.0f, 0.0f };
        s32 col[4];
        rn_rgba_lanes(v->rgba, col);
        rn_mesh_rec(recs + 64u * i, pos, NULL, col, uv);
        idx[i] = i;
    }
    if (ps2_vk_mesh_store(recs, wt.h.n, idx, wt.h.n, &vbase, &ifirst) != 0) return 0;
    rn_mesh_block_init(&b);
    memcpy(b.m, wt.h.rows, sizeof b.m);
    for (int i = 0; i < 4; i++) b.m[4 * i + 2] = b.m[4 * i + 3] * (float)wt.h.z;
    b.fog0 = 255.0f;
    ps2_gs_native_state(&st);
    ps2_vk_draw_mesh(&st, &b, vbase, ifirst, wt.h.n, 0);
    st_wt.tris += wt.h.n / 3u;
    return 1;
}

int rn_worldtri_claim_prim(const struct ps2_vk_state *st, int kind,
                           const struct ps2_vk_vertex *v, int n) {
    (void)st; (void)kind; (void)v; (void)n;
    if (!wt.drawn) {
        wt.drawn = draw_worldtri() ? 1 : -1;
        if (wt.drawn > 0) st_wt.native++;
        else st_wt.fallback++;
    }
    if (wt.drawn < 0) return 0;
    st_wt.claimed++;
    return 1;
}

void rn_worldtri_report(void) {
    if (!st_wt.batches) return;
    ps2_log("rn: native world triangles -- %llu batches (%llu triangles): %llu drawn by "
            "mesh.vert (%llu primitives claimed), %llu left emulated",
            (unsigned long long)st_wt.batches, (unsigned long long)st_wt.tris,
            (unsigned long long)st_wt.native, (unsigned long long)st_wt.claimed,
            (unsigned long long)st_wt.fallback);
}
