#include "ps2_hook.h"
#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F_SUN_FLARE 0x00118BC8u
static const u32 sun_flare_w[2] = { 0x27BDFB70u, 0xFFB70458u };

static struct { u32 obj, pkt, gp; float arg; } fl_in;
static struct { u64 fans, rewritten, compared, matched, differed, skipped, unproven; } st_fl;
static int fl_proven;

static float rdf(u32 a) {
    u32 v = ps2_r32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void wrf(u32 a, float f) {
    u32 v;
    memcpy(&v, &f, 4);
    ps2_w32(a, v);
}

typedef struct { float x, y, s, t; } fl_vtx;

static u32 clip_edge(const fl_vtx *in, u32 n, fl_vtx *out, int axis, float bound, int keep_le) {
    u32 m = 0;
    for (u32 i = 0; i < n; i++) {
        const fl_vtx *a = &in[i], *b = &in[(i + 1u) % n];
        float va = axis ? a->y : a->x, vb = axis ? b->y : b->x;
        int ina = keep_le ? va <= bound : va >= bound;
        int inb = keep_le ? vb <= bound : vb >= bound;
        if (ina) out[m++] = *a;
        if (ina != inb) {
            float t = (bound - va) / (vb - va);
            fl_vtx c;
            c.x = a->x + (b->x - a->x) * t;
            c.y = a->y + (b->y - a->y) * t;
            c.s = a->s + (b->s - a->s) * t;
            c.t = a->t + (b->t - a->t) * t;
            if (axis) c.y = bound; else c.x = bound;
            out[m++] = c;
        }
    }
    return m;
}

static int tap_flare_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    fl_in.obj = (u32)ctx->r[4].ud[0];
    fl_in.pkt = (u32)ctx->r[5].ud[0];
    fl_in.gp = (u32)ctx->r[28].ud[0];
    fl_in.arg = ctx->f[12].f;
    return 0;
}

static int tap_flare(ps2_ctx *ctx, void *u) {
    static const float st_outer[8][2] = { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 0, 0 },
                                          { 1, 0 }, { 0, 0 }, { 0, 1 }, { 0, 0 } };
    u32 end = (u32)ctx->r[2].ud[0];
    u32 fan = fl_in.pkt + 0x70u;
    u32 sp = (u32)ctx->r[29].ud[0] - 0x490u;
    u32 obj = fl_in.obj, gp = fl_in.gp;
    float cx, cy, f0, sx, sy, xmax, ymax;
    fl_vtx ring[8], a[64], b[64];
    u32 n, old_count, new_count;
    (void)u;
    if (end == fl_in.pkt) return 0;
    if (end < fan + 48u || (ps2_r32(fan + 20u) != 0x112AC000u)
        || (ps2_r32(fan) & 0xF0000000u) != 0x10000000u) {
        st_fl.skipped++;
        return 0;
    }
    st_fl.fans++;
    old_count = (end - (fan + 48u)) / 32u;

    cx = rdf(sp + 0x1A0u);
    cy = rdf(sp + 0x1A4u);
    f0 = rdf(obj + 0x68u) * 4096.0f;
    f0 = f0 * rdf(gp - 0x7EF0u);
    f0 = f0 * (fl_in.arg * 0.001953125f);
    sx = f0 * rdf(obj + 0x48u);
    sy = f0 * rdf(obj + 0x4Cu);
    xmax = rdf(gp - 0x7EE4u);
    ymax = rdf(gp - 0x7EE0u);
    for (u32 k = 0; k < 8u; k++) {
        float dx = rdf(sp + 0x160u + 8u * k), dy = rdf(sp + 0x164u + 8u * k);
        ring[k].x = cx + sx * dx;
        ring[k].y = cy + sy * dy;
        ring[k].s = st_outer[k][0];
        ring[k].t = st_outer[k][1];
    }

    {
        int centre_ok = ps2_r32(fan + 48u + 16u) == ((u32)(s32)cx | ((u32)(s32)cy << 16))
                     && rdf(fan + 48u) == 1.0f && rdf(fan + 52u) == 1.0f;
        if (centre_ok) {
            for (u32 j = 1; j < old_count && j < 64u && !fl_proven; j++) {
                u32 base = fan + 48u + 32u * j, xy = ps2_r32(base + 16u);
                float s = rdf(base), t = rdf(base + 4u);
                for (u32 k = 0; k < 8u; k++) {
                    const fl_vtx *v = &ring[k];
                    if (v->x > xmax || v->x < 28288.0f || v->y > ymax || v->y < 28800.0f)
                        continue;
                    if (xy == ((u32)(s32)v->x | ((u32)(s32)v->y << 16)) && s == v->s
                        && t == v->t) {
                        fl_proven = 1;
                        break;
                    }
                }
            }
        }
    }
    {
        int inside = 1;
        for (u32 k = 0; k < 8u; k++)
            if (ring[k].x > xmax || ring[k].x < 28288.0f || ring[k].y > ymax
                || ring[k].y < 28800.0f)
                inside = 0;
        if (inside && old_count == 10u) {
            int same = 1;
            for (u32 k = 0; k < 9u && same; k++) {
                const fl_vtx *v = &ring[k % 8u];
                u32 base = fan + 48u + 32u * (k + 1u);
                u32 xy = ps2_r32(base + 16u);
                u32 want = (u32)(s32)v->x | ((u32)(s32)v->y << 16);
                same = xy == want && rdf(base) == v->s && rdf(base + 4u) == v->t;
            }
            st_fl.compared++;
            if (same) st_fl.matched++;
            else {
                static int shown;
                st_fl.differed++;
                if (shown++ < 4)
                    ps2_log("rn-fix: flare inputs disagree with the engine's unclipped fan "
                            "(centre %.1f,%.1f scale %g,%g); the fix is off", (double)cx,
                            (double)cy, (double)sx, (double)sy);
                fl_proven = 0;
                return 0;
            }
        }
    }

    if (!fl_proven) {
        st_fl.unproven++;
        return 0;
    }
    n = clip_edge(ring, 8u, a, 0, xmax, 1);
    n = clip_edge(a, n, b, 0, 28288.0f, 0);
    n = clip_edge(b, n, a, 1, ymax, 1);
    n = clip_edge(a, n, b, 1, 28800.0f, 0);
    if (n < 2u || n > 60u) return 0;

    new_count = n + 2u;
    ps2_w32(fan, (2u * new_count + 2u) | 0x10000000u);
    ps2_w32(fan + 12u, (2u * new_count + 2u) | 0x50000000u);
    ps2_w32(fan + 16u, (2u * new_count + 1u) | 0x8000u);
    for (u32 k = 0; k < new_count; k++) {
        u32 base = fan + 48u + 32u * k;
        fl_vtx v;
        if (k == 0u) { v.x = cx; v.y = cy; v.s = 1.0f; v.t = 1.0f; }
        else v = b[(k - 1u) % n];
        wrf(base, v.s);
        wrf(base + 4u, v.t);
        ps2_w32(base + 8u, 2u);
        ps2_w32(base + 12u, 0u);
        ps2_w32(base + 16u, (u32)(s32)v.x | ((u32)(s32)v.y << 16));
        ps2_w32(base + 20u, 0xFF000000u);
        ps2_w32(base + 24u, 4u);
        ps2_w32(base + 28u, 0u);
    }
    ctx->r[2].ud[0] = (u64)(s64)(s32)(fan + 48u + 32u * new_count);
    st_fl.rewritten++;
    return 0;
}

void rn_fixes_init(void) {
    const char *e = getenv("PS2_FIX_FLARE");
    if (e && *e == '0') return;
    if (ps2_r32(F_SUN_FLARE) != sun_flare_w[0] || ps2_r32(F_SUN_FLARE + 4u) != sun_flare_w[1]) {
        ps2_log("rn-fix: the sun flare %08X does not match; not fixed", F_SUN_FLARE);
        return;
    }
    if (ps2_hook_before(F_SUN_FLARE, tap_flare_enter, NULL, 110, "rn-fixes") < 0
        || ps2_hook_after(F_SUN_FLARE, tap_flare, NULL, 110, "rn-fixes") < 0)
        ps2_log("rn-fix: the hook layer refused the sun flare fix");
}

void rn_fixes_report(void) {
    if (!st_fl.fans && !st_fl.skipped) return;
    ps2_log("rn-fix: sun flare -- %llu fans seen, %llu rewritten with proper clipping; "
            "%llu unclipped ones compared with the engine's, %llu identical, %llu not; "
            "%llu not recognised as the flare's fan, %llu left as the engine wrote "
            "them because its inputs were not proven yet",
            (unsigned long long)st_fl.fans,
            (unsigned long long)st_fl.rewritten, (unsigned long long)st_fl.compared,
            (unsigned long long)st_fl.matched, (unsigned long long)st_fl.differed,
            (unsigned long long)st_fl.skipped, (unsigned long long)st_fl.unproven);
}
