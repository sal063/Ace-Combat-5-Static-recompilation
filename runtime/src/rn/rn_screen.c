#include "rn_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_screen_on;

typedef struct { u32 lo, hi; const char *what; } screen_site;

static const screen_site sites[] = {
    { 0x0016171Cu, 0x0016171Du, "scene fade" },
    { 0x001609A8u, 0x001609A9u, "impostor target clears" },
    { 0x001D059Cu, 0x001D059Du, "cloud impostor puffs" },
    { 0x001D03E0u, 0x001D03E1u, "cloud impostors (no intent)" },
    { 0x001CD2CCu, 0x001CD2CDu, "cloud planes (no intent)" },
    { 0x0012CD88u, 0x0012CD89u, "self-shadow passes" },
    { 0x00113EC8u, 0x00114180u, "sky passes" },
    { 0x00114580u, 0x0011467Cu, "sky passes (sub_114580)" },
    { 0x0011AAF0u, 0x0011B82Cu, "sun occlusion chain" },
    { 0x00118BC8u, 0x00119B74u, "sun flare" },
    { 0x001B0680u, 0x001B09ECu, "sun glare" },
    { 0x002F0628u, 0x002F0629u, "movie frame clear" },
    { 0x00205E28u, 0x0026BDCCu, "effect system" },
};
#define N_SITES (sizeof sites / sizeof sites[0])

static u8 decided[RN_EMIT_MAX];
static u64 st_by_site[N_SITES + 2];

static int skipped(u32 site) {
    static char list[256];
    static int init;
    const char *p;
    if (!init) {
        const char *e = getenv("PS2_RN_SCREEN_SKIP");
        init = 1;
        if (e) snprintf(list, sizeof list, "%s", e);
    }
    for (p = list; *p; ) {
        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        if ((u32)v == site) return 1;
        p = *end ? end + 1 : end;
    }
    return 0;
}

static u8 site_of[RN_EMIT_MAX];

static int is_screen_emitter(u32 e) {
    if (e >= RN_EMIT_MAX) return 0;
    if (!decided[e]) {
        decided[e] = 2;
        if (e == RN_EMIT_DRAWCTRL) {
            decided[e] = skipped(0) ? 2 : 1;
        } else if (e == RN_EMIT_OFFCHAIN) {
            decided[e] = skipped(3) ? 2 : 1;
            site_of[e] = (u8)(N_SITES + 1u);
        } else if (e >= RN_EMIT_FIRST) {
            const rn_emitter *em = rn_emitter_get(e);
            for (u32 i = 0; em && i < N_SITES; i++)
                if (em->site >= sites[i].lo && em->site < sites[i].hi
                    && !skipped(em->site) && !skipped(sites[i].lo)) {
                    decided[e] = 1;
                    site_of[e] = (u8)(i + 1u);
                    break;
                }
        }
    }
    return decided[e] == 1;
}

#define RUN_MAX 4096u

typedef struct {
    int kind;
    ps2_vk_vertex v[6];
    float x0, y0, x1, y1, s0, t0, s1, t1;
} held;

static int topo_of(int kind) {
    return kind == RN_PRIM_LINE ? PS2_VK_LINES
         : kind == RN_PRIM_POINT ? PS2_VK_POINTS : PS2_VK_TRIANGLES;
}

static struct {
    int n;
    int topo;
    ps2_vk_state st;
    held p[RUN_MAX];
    int flushing;
} run;

static struct {
    u64 prims, sprites, merged, tris, draws, fallback, lines_points, other;
} st_scr;

static void sprite_rect(held *h) {
    const ps2_vk_vertex *a = &h->v[0], *c = &h->v[2];
    float x0 = a->x, x1 = c->x, y0 = a->y, y1 = c->y;
    float s0 = a->s, s1 = c->s, t0 = a->t, t1 = c->t;
    if (x1 < x0) { float k = x0; x0 = x1; x1 = k; k = s0; s0 = s1; s1 = k; }
    if (y1 < y0) { float k = y0; y0 = y1; y1 = k; k = t0; t0 = t1; t1 = k; }
    float px0 = ceilf(x0 - 0.5f), px1 = ceilf(x1 - 0.5f);
    float py0 = ceilf(y0 - 0.5f), py1 = ceilf(y1 - 0.5f);
    float ds = x1 != x0 ? (s1 - s0) / (x1 - x0) : 0.0f;
    float dt = y1 != y0 ? (t1 - t0) / (y1 - y0) : 0.0f;
    h->x0 = px0; h->x1 = px1; h->y0 = py0; h->y1 = py1;
    h->s0 = s0 + (px0 - x0) * ds;
    h->s1 = s1 + (px1 - x1) * ds;
    h->t0 = t0 + (py0 - y0) * dt;
    h->t1 = t1 + (py1 - y1) * dt;
}

static int continues(const held *p, const held *h) {
    const float eps = 1.0f / 64.0f;
    if (p->kind != RN_PRIM_SPRITE || h->kind != RN_PRIM_SPRITE) return 0;
    if (p->y0 != h->y0 || p->y1 != h->y1 || p->x1 != h->x0) return 0;
    if (fabsf(p->s1 - h->s0) > eps || fabsf(p->t0 - h->t0) > eps
        || fabsf(p->t1 - h->t1) > eps) return 0;
    if (memcmp(&p->v[0].r, &h->v[0].r, 4 * sizeof(float))) return 0;
    if (p->v[0].z != h->v[0].z || p->v[0].q != h->v[0].q || p->v[0].fog != h->v[0].fog)
        return 0;
    if (p->v[0].round_uv != h->v[0].round_uv) return 0;
    {   float pk = (p->s1 - p->s0) / (p->x1 - p->x0), hk = (h->s1 - h->s0) / (h->x1 - h->x0);
        if (fabsf(pk - hk) > 1.0f / 4096.0f) return 0; }
    return 1;
}

static void rec_of(u8 *rec, float x, float y, const ps2_vk_vertex *v, float s, float t) {
    float pos[4] = { x, y, v->z, 1.0f };
    s32 nrm[4] = { (s32)v->round_uv, 0, 0, 0 };
    s32 col[4] = { (s32)v->r, (s32)v->g, (s32)v->b, (s32)v->a };
    float uv[4] = { s, t, v->q, v->fog };
    rn_mesh_rec(rec, pos, nrm, col, uv);
}

static void hand_back(void) {
    for (int i = 0; i < run.n; i++) {
        const held *h = &run.p[i];
        if (h->kind == RN_PRIM_SPRITE) {
            ps2_vk_draw(PS2_VK_TRIANGLES, &run.st, h->v, 3);
            ps2_vk_draw(PS2_VK_TRIANGLES, &run.st, h->v + 3, 3);
        } else if (h->kind == RN_PRIM_LINE) {
            ps2_vk_draw(PS2_VK_LINES, &run.st, h->v, 2);
        } else if (h->kind == RN_PRIM_POINT) {
            ps2_vk_draw(PS2_VK_POINTS, &run.st, h->v, 1);
        } else {
            ps2_vk_draw(PS2_VK_TRIANGLES, &run.st, h->v, 3);
        }
    }
    st_scr.fallback += (u64)run.n;
}

static void flush(void) {
    static u8 recs[RUN_MAX * 4u * 64u];
    static u32 idx[RUN_MAX * 6u];
    u32 nv = 0, ni = 0, vbase, ifirst;
    ps2_vk_mesh_block b;
    if (!run.n || run.flushing) return;
    run.flushing = 1;
    for (int i = 0; i < run.n; i++) {
        const held *h = &run.p[i];
        if (h->kind == RN_PRIM_SPRITE) {
            const ps2_vk_vertex *c = &h->v[2];
            rec_of(recs + 64u * (nv + 0u), h->x0, h->y0, c, h->s0, h->t0);
            rec_of(recs + 64u * (nv + 1u), h->x1, h->y0, c, h->s1, h->t0);
            rec_of(recs + 64u * (nv + 2u), h->x1, h->y1, c, h->s1, h->t1);
            rec_of(recs + 64u * (nv + 3u), h->x0, h->y1, c, h->s0, h->t1);
            idx[ni++] = nv; idx[ni++] = nv + 1u; idx[ni++] = nv + 2u;
            idx[ni++] = nv; idx[ni++] = nv + 2u; idx[ni++] = nv + 3u;
            nv += 4u;
        } else {
            int nk = h->kind == RN_PRIM_LINE ? 2 : h->kind == RN_PRIM_POINT ? 1 : 3;
            for (int k = 0; k < nk; k++) {
                const ps2_vk_vertex *v = &h->v[k];
                rec_of(recs + 64u * nv, v->x, v->y, v, v->s, v->t);
                idx[ni++] = nv++;
            }
        }
    }
    if (!ps2_vk_mesh_available()
        || ps2_vk_mesh_store(recs, nv, idx, ni, &vbase, &ifirst) != 0) {
        hand_back();
    } else {
        rn_mesh_block_init(&b);
        b.prog = 5u;
        ps2_vk_draw_mesh_topo(&run.st, &b, vbase, ifirst, ni, PS2_VK_MESH_GSFRAG, run.topo);
        st_scr.draws++;
    }
    run.n = 0;
    run.flushing = 0;
}

static void before_record(void) {
    if (run.n && !run.flushing) flush();
}

void rn_screen_flush(void) { before_record(); }

static int claim(const struct ps2_vk_state *st, int kind,
                 const struct ps2_vk_vertex *v, int n);

int rn_screen_claim_prim(const struct ps2_vk_state *st, int kind,
                         const struct ps2_vk_vertex *v, int n) {
    if (!rn_screen_on || !is_screen_emitter(st->emitter)) return 0;
    if (rn_vu_kicking || rn_census_native_prog) return 0;
    st_by_site[st->emitter < RN_EMIT_MAX ? site_of[st->emitter] : 0]++;
    return claim(st, kind, v, n);
}

int rn_screen_claim_any(const struct ps2_vk_state *st, int kind,
                        const struct ps2_vk_vertex *v, int n) {
    if (!rn_screen_on || rn_vu_kicking) return 0;
    st_scr.other++;
    return claim(st, kind, v, n);
}

static int claim(const struct ps2_vk_state *st, int kind,
                 const struct ps2_vk_vertex *v, int n) {
    held h;
    if (!ps2_vk_mesh_available()) return 0;
    if (run.n && (run.n >= (int)RUN_MAX || run.topo != topo_of(kind)
                  || memcmp(&run.st, st, sizeof *st)))
        flush();
    if (kind == RN_PRIM_LINE || kind == RN_PRIM_POINT) st_scr.lines_points++;
    memset(&h, 0, sizeof h);
    h.kind = kind;
    memcpy(h.v, v, sizeof(ps2_vk_vertex) * (size_t)(n < 6 ? n : 6));
    st_scr.prims++;
    if (kind == RN_PRIM_SPRITE) {
        sprite_rect(&h);
        st_scr.sprites++;
        if (h.x1 <= h.x0 || h.y1 <= h.y0) return 1;
        if (run.n && continues(&run.p[run.n - 1], &h)) {
            held *p = &run.p[run.n - 1];
            p->x1 = h.x1;
            p->s1 = h.s1;
            st_scr.merged++;
            return 1;
        }
    } else if (kind == RN_PRIM_TRI) {
        st_scr.tris++;
    }
    if (!run.n) { run.st = *st; run.topo = topo_of(kind); }
    run.p[run.n++] = h;
    return 1;
}

void rn_screen_init(void) {
    const char *e = getenv("PS2_RN_SCREEN");
    rn_screen_on = !(e && *e == '0');
    if (rn_screen_on) ps2_vk_before_record = before_record;
}

void rn_screen_report(void) {
    if (!st_scr.prims) return;
    if (st_scr.other)
        ps2_log("rn:    screen pass %-28s %llu primitives", "front end, not 2D",
                (unsigned long long)st_scr.other);
    for (u32 i = 0; i <= N_SITES + 1u; i++)
        if (st_by_site[i])
            ps2_log("rn:    screen pass %-28s %llu primitives",
                    i == 0 ? "DrawCtrl flush" : i > N_SITES ? "off-chain kicks (movies)"
                           : sites[i - 1].what,
                    (unsigned long long)st_by_site[i]);
    ps2_log("rn: native screen passes -- %llu primitives (%llu sprites, %llu merged into "
            "the one before, %llu triangles) in %llu mesh draws; %llu handed back for "
            "arena space; %llu of the primitives lines and points",
            (unsigned long long)st_scr.prims, (unsigned long long)st_scr.sprites,
            (unsigned long long)st_scr.merged, (unsigned long long)st_scr.tris,
            (unsigned long long)st_scr.draws, (unsigned long long)st_scr.fallback,
            (unsigned long long)st_scr.lines_points);
}
