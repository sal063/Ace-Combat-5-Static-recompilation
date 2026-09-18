#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_sky_on;

#define SKY_RINGS_MAX 64u
#define SKY_SEGS 32u

static struct {
    int active;
    int drawn;
    u32 layers_drawn;
    u32 vbase, ifirst, ni;
    rn_int_skydome d;
    rn_sky_ring ring[SKY_RINGS_MAX];
} sky;

static struct { u64 domes, hazes, native, fallback, claimed, layers; } st_sky;

void rn_sky_init(void) {
    const char *e = getenv("PS2_RN_SKY");
    rn_sky_on = !(e && *e == '0');
    if (rn_sky_on) ps2_log("rn: native sky on (PS2_RN_SKY=0 turns it off)");
}

void rn_sky_intent(const u8 *rec, u32 len) {
    rn_intent_hdr h;
    if (!rn_sky_on || len < sizeof h + sizeof sky.d) return;
    memcpy(&h, rec, sizeof h);
    memcpy(&sky.d, rec + sizeof h, sizeof sky.d);
    if (sky.d.rings < 3u || sky.d.rings > SKY_RINGS_MAX || sky.d.segs != SKY_SEGS
        || len < sizeof h + sizeof sky.d + sky.d.rings * sizeof sky.ring[0])
        return;
    if (sky.d.pass == RN_SKY_HAZE
        && (sky.d.ring0 < 1u || sky.d.ring1 + 1u >= sky.d.rings || sky.d.ring0 >= sky.d.ring1
            || sky.d.layers > 8u))
        return;
    memcpy(sky.ring, rec + sizeof h + sizeof sky.d, sky.d.rings * sizeof sky.ring[0]);
    sky.active = 1;
    sky.drawn = 0;
    sky.layers_drawn = 0;
    if (sky.d.pass == RN_SKY_HAZE) st_sky.hazes++;
    else st_sky.domes++;
}

void rn_sky_end(void) {
    sky.active = 0;
}

int rn_sky_active(void) {
    return sky.active;
}

static void put_point(u8 *rec, float x, float y, float z, u32 rgba) {
    float pos[4] = { x, y, z, 1.0f };
    s32 col[4];
    rn_rgba_lanes(rgba, col);
    rn_mesh_rec(rec, pos, NULL, col, NULL);
}

static int store_mesh(void) {
    static u8 recs[(2u + (SKY_RINGS_MAX - 2u) * SKY_SEGS) * 64u];
    static u32 idx[(SKY_RINGS_MAX - 1u) * SKY_SEGS * 6u];
    const u32 nr = sky.d.rings, S = SKY_SEGS;
    const int haze = sky.d.pass == RN_SKY_HAZE;
    const u32 first = haze ? sky.d.ring0 : 0u, last = haze ? sky.d.ring1 : nr - 1u;
    u32 nv = 0, ni = 0;
    for (u32 r = first; r <= last; r++) {
        float R = sky.ring[r].radius * 65536.0f, H = sky.ring[r].height * 65536.0f;
        u32 n = (!haze && (r == 0u || r == nr - 1u)) ? 1u : S;
        for (u32 k = 0; k < n; k++)
            put_point(recs + nv++ * 64u, sky.d.seg[2u * k] * R, H,
                      sky.d.seg[2u * k + 1u] * R, sky.ring[r].rgba);
    }
    for (u32 k = 0; k < S; k++) {
        u32 k1 = (k + 1u) % S;
        u32 band0 = haze ? 0u : 1u, bands = haze ? last - first : nr - 3u;
        if (!haze) {
            const u32 apex_bottom = 1u + (nr - 2u) * S;
            idx[ni++] = 0u; idx[ni++] = 1u + k; idx[ni++] = 1u + k1;
            idx[ni++] = apex_bottom;
            idx[ni++] = 1u + (nr - 3u) * S + k;
            idx[ni++] = 1u + (nr - 3u) * S + k1;
        }
        for (u32 j = 0; j < bands; j++) {
            u32 a = band0 + j * S, c = a + S;
            idx[ni++] = a + k;  idx[ni++] = c + k;  idx[ni++] = a + k1;
            idx[ni++] = c + k;  idx[ni++] = c + k1; idx[ni++] = a + k1;
        }
    }
    if (ps2_vk_mesh_store(recs, nv, idx, ni, &sky.vbase, &sky.ifirst) != 0) return 0;
    sky.ni = ni;
    return 1;
}

static void draw_pass(const ps2_vk_state *st, int layer) {
    ps2_vk_mesh_block b;
    ps2_vk_state s = *st;
    rn_mesh_block_init(&b);
    memcpy(b.m, sky.d.screen, sizeof b.m);
    memcpy(b.c, sky.d.clip, sizeof b.c);
    b.fog0 = 255.0f;
    b.fog1 = 0.0f;
    if (layer < 0)
        b.zscale = 0.0f;
    else
        for (int i = 0; i < 4; i++) b.m[4 * i + 2] = b.m[4 * i + 3] * (float)sky.d.z[layer];
    b.clip = 1;
    s.tme = 0;
    ps2_vk_draw_mesh(&s, &b, sky.vbase, sky.ifirst, sky.ni, 0);
}

int rn_sky_claim_prim(const struct ps2_vk_state *st, int kind,
                      const struct ps2_vk_vertex *v, int n) {
    (void)kind; (void)v; (void)n;
    if (!sky.drawn) {
        sky.drawn = ps2_vk_mesh_available() && store_mesh() ? 1 : -1;
        if (sky.drawn > 0) st_sky.native++;
        else st_sky.fallback++;
        if (sky.drawn > 0 && sky.d.pass == RN_SKY_DOME) draw_pass(st, -1);
    }
    if (sky.drawn < 0) return 0;
    if (sky.d.pass == RN_SKY_HAZE) {
        u32 layer = st->alpha_fix / 16u;
        if (layer >= sky.d.layers || st->alpha_fix % 16u) return 0;
        if (!(sky.layers_drawn & (1u << layer))) {
            sky.layers_drawn |= 1u << layer;
            st_sky.layers++;
            draw_pass(st, (int)layer);
        }
    }
    st_sky.claimed++;
    return 1;
}

void rn_sky_report(void) {
    if (!st_sky.domes && !st_sky.hazes) return;
    ps2_log("rn: native sky -- %llu domes and %llu haze passes (%llu layers): %llu drawn "
            "by mesh.vert (%llu strip triangles claimed), %llu left emulated",
            (unsigned long long)st_sky.domes, (unsigned long long)st_sky.hazes,
            (unsigned long long)st_sky.layers, (unsigned long long)st_sky.native,
            (unsigned long long)st_sky.claimed, (unsigned long long)st_sky.fallback);
}
