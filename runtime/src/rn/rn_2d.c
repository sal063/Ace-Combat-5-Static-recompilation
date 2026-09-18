#include "rn_int.h"
#include "ps2_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_claims_on;
static int on_2d;

typedef struct {
    ps2_vk_state st;
    int kind, n;
    int reject;
    ps2_vk_vertex v[6];
} held_prim;

static struct {
    int active;
    u32 site, emitter, prim, count, flags;
    u64 tex0;
    held_prim *held;
    u32 n, cap;
    int reject;
} el;

enum { REJ_NONE = 0, REJ_TARGET, REJ_TEXRT, REJ_ZWRITE, REJ_DATE, REJ_DSTALPHA,
       REJ_COLCLIP, REJ_MASK, REJ_N };
static const char *rej_name[REJ_N] = {
    "accepted", "not slot 0", "reads a render target", "writes Z",
    "destination alpha test", "blend uses destination alpha", "wrapping blend",
    "partial frame mask" };

static struct {
    u64 elements, whole, prims, rejected[REJ_N], unclosed, reanchored;
    u64 out_of_space;
    u64 to_screen;
} st2d;

static int reject_reason(const ps2_vk_state *st) {
    if (st->rt != 0u) return REJ_TARGET;
    if (st->tex_rt) return REJ_TEXRT;
    if (st->zwrite && st->zte) return REJ_ZWRITE;
    if (st->date) return REJ_DATE;
    if (st->abe && st->alpha_c == 1u) return REJ_DSTALPHA;
    if (st->colclip) return REJ_COLCLIP;
    if (st->fb_mask && st->fb_mask != 0xFF000000u) return REJ_MASK;
    return REJ_NONE;
}

static void hand_over(const held_prim *h, int native) {
    void (*draw)(int, const ps2_vk_state *, const ps2_vk_vertex *, int) =
        native ? ps2_vk_draw_2d : ps2_vk_draw;
    switch (h->kind) {
    case RN_PRIM_SPRITE:
        draw(PS2_VK_TRIANGLES, &h->st, h->v, 3);
        draw(PS2_VK_TRIANGLES, &h->st, h->v + 3, 3);
        break;
    case RN_PRIM_TRI:   draw(PS2_VK_TRIANGLES, &h->st, h->v, 3); break;
    case RN_PRIM_LINE:  draw(PS2_VK_LINES, &h->st, h->v, 2); break;
    default:            draw(PS2_VK_POINTS, &h->st, h->v, 1); break;
    }
}

static void reanchor(void) {
    float x0 = 1e9f, x1 = -1e9f, cx, gx, s;
    u32 fbw = 0;
    if (!ps2_cfg.widescreen || ps2_cfg.hud_layout != PS2_HUD_CENTERED) return;
    if (el.flags & RN_G_WORLD) return;
    if (!(ps2_vk_display_aspect > 1.34f)) return;
    for (u32 i = 0; i < el.n; i++) {
        const held_prim *h = &el.held[i];
        if (h->st.rt != 0u) continue;
        if (h->st.fb_w > fbw) fbw = h->st.fb_w;
        for (int k = 0; k < h->n; k++) {
            if (h->v[k].x < x0) x0 = h->v[k].x;
            if (h->v[k].x > x1) x1 = h->v[k].x;
        }
    }
    if (!fbw || x1 < x0 || x1 - x0 >= 0.9f * (float)fbw) return;
    s = (4.0f / 3.0f) / ps2_vk_display_aspect;
    gx = (float)fbw * 0.5f;
    cx = gx + 0.5f;
    for (u32 i = 0; i < el.n; i++) {
        held_prim *h = &el.held[i];
        if (h->st.rt != 0u) continue;
        if (h->st.tex_point) h->st.tex_point = 2;
        for (int k = 0; k < h->n; k++) {
            h->v[k].x = cx + (h->v[k].x - cx) * s;
            h->v[k].round_uv &= PS2_VK_RUV_SPRITE;
        }
        {
            float l = gx + ((float)h->st.scissor[0] - gx) * s;
            float r = gx + ((float)h->st.scissor[1] + 1.0f - gx) * s;
            h->st.scissor[0] = (s32)floorf(l);
            h->st.scissor[1] = (s32)ceilf(r) - 1;
        }
    }
    st2d.reanchored++;
}

static void element_end(void) {
    int native;
    if (!el.active) return;
    el.active = 0;
    if (!el.n) return;
    native = el.reject == REJ_NONE;
    {   static FILE *lf;
        static int init;
        if (!init) {
            const char *p = getenv("PS2_RN_2D_LOG");
            init = 1;
            lf = p && *p ? fopen(p, "w") : NULL;
        }
        if (lf) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (u32 i = 0; i < el.n; i++)
                for (int k = 0; k < el.held[i].n; k++) {
                    const ps2_vk_vertex *v = &el.held[i].v[k];
                    if (v->x < x0) x0 = v->x;
                    if (v->x > x1) x1 = v->x;
                    if (v->y < y0) y0 = v->y;
                    if (v->y > y1) y1 = v->y;
                }
            fprintf(lf, "%u %08X %u %.1f %.1f %.1f %.1f %u %d\n",
                    ps2_gs_frame_count, el.site, el.emitter, x0, y0, x1, y1,
                    el.n, native);
        }
    }
    st2d.elements++;
    st2d.whole += native;
    reanchor();
    for (u32 i = 0; i < el.n; i++) {
        const held_prim *h = &el.held[i];
        st2d.rejected[h->reject]++;
        if (h->reject != REJ_NONE && rn_screen_claim_any(&h->st, h->kind, h->v, h->n)) {
            st2d.to_screen++;
            continue;
        }
        hand_over(h, h->reject == REJ_NONE);
    }
    el.n = 0;
}

static int claim_2d(const struct ps2_vk_state *st, int kind,
                    const struct ps2_vk_vertex *v, int n) {
    held_prim *h;
    if (el.n == el.cap) {
        u32 cap = el.cap ? el.cap * 2u : 256u;
        held_prim *grown = cap <= 65536u
                         ? (held_prim *)realloc(el.held, cap * sizeof *grown) : NULL;
        if (!grown) {
            st2d.out_of_space++;
            element_end();
            return 0;
        }
        el.held = grown;
        el.cap = cap;
    }
    h = &el.held[el.n++];
    h->st = *st;
    h->kind = kind;
    h->n = n > 6 ? 6 : n;
    memcpy(h->v, v, sizeof(ps2_vk_vertex) * (size_t)h->n);
    h->reject = reject_reason(st);
    if (el.reject == REJ_NONE) el.reject = h->reject;
    st2d.prims++;
    return 1;
}

static u8 fe_decided[RN_EMIT_MAX];
static int fe_on = -1;
static struct { u64 prims, as2d, screen; } st_fe;

static const struct { u32 lo, hi; } fe_ranges[] = {
    { 0x002886D8u, 0x002EA730u },
    { 0x0012BF1Cu, 0x0012BF1Du },
    { 0x0012BFA8u, 0x0012BFA9u },
    { 0x00161150u, 0x00161151u },
};

static int frontend_emitter(u32 e) {
    if (!fe_on || e < RN_EMIT_FIRST || e >= RN_EMIT_MAX) return 0;
    if (!fe_decided[e]) {
        const rn_emitter *em = rn_emitter_get(e);
        fe_decided[e] = 2;
        for (u32 i = 0; em && i < sizeof fe_ranges / sizeof fe_ranges[0]; i++)
            if (em->site >= fe_ranges[i].lo && em->site < fe_ranges[i].hi) {
                fe_decided[e] = 1;
                break;
            }
    }
    return fe_decided[e] == 1;
}

static int claim_frontend(const struct ps2_vk_state *st, int kind,
                          const struct ps2_vk_vertex *v, int n) {
    held_prim h;
    int why = reject_reason(st);
    st_fe.prims++;
    if (why == REJ_ZWRITE && st->ztst == 1u) why = REJ_NONE;
    if (on_2d && why == REJ_NONE) {
        h.st = *st;
        h.kind = kind;
        h.n = n > 6 ? 6 : n;
        memcpy(h.v, v, sizeof(ps2_vk_vertex) * (size_t)h.n);
        hand_over(&h, 1);
        st_fe.as2d++;
        return 1;
    }
    if (rn_screen_claim_any(st, kind, v, n)) { st_fe.screen++; return 1; }
    return 0;
}

int rn_claim_prim_slow(const struct ps2_vk_state *st, int kind,
                       const struct ps2_vk_vertex *v, int n) {
    if (el.active) return claim_2d(st, kind, v, n);
    if (rn_sky_active()) return rn_sky_claim_prim(st, kind, v, n);
    if (rn_tri3d_active()) return rn_tri3d_claim_prim(st, kind, v, n);
    if (rn_billboard_active()) return rn_billboard_claim_prim(st, kind, v, n);
    if (rn_worldtri_active()) return rn_worldtri_claim_prim(st, kind, v, n);
    if (rn_sun_on && rn_sun_claim_prim(st, kind, v, n)) return 1;
    if (!rn_vu_kicking && !rn_census_native_prog && frontend_emitter(st->emitter))
        return claim_frontend(st, kind, v, n);
    if (rn_screen_on) return rn_screen_claim_prim(st, kind, v, n);
    return 0;
}

void rn_gs_intent(const u8 *rec, u32 len) {
    rn_intent_hdr h;
    if (!rec || len < sizeof h) return;
    memcpy(&h, rec, sizeof h);
    switch (h.kind) {
    case RN_INT_PRIM2D: {
        rn_int_prim2d b;
        if (!on_2d || len < sizeof h + sizeof b) return;
        if (el.active) { st2d.unclosed++; element_end(); }
        memcpy(&b, rec + sizeof h, sizeof b);
        el.active = 1;
        el.site = h.site;
        el.emitter = h.emitter;
        el.flags = h.flags;
        el.prim = b.prim;
        el.count = b.count;
        el.tex0 = b.tex0;
        el.n = 0;
        el.reject = REJ_NONE;
        break;
    }
    case RN_INT_GROUP2D:
        if (!on_2d) return;
        if (el.active) { st2d.unclosed++; element_end(); }
        el.active = 1;
        el.site = h.site;
        el.emitter = h.emitter;
        el.flags = h.flags;
        el.prim = 0;
        el.count = 0;
        el.tex0 = 0;
        el.n = 0;
        el.reject = REJ_NONE;
        break;
    case RN_INT_SKYDOME:
        if (el.active) { st2d.unclosed++; element_end(); }
        rn_sky_intent(rec, len);
        break;
    case RN_INT_TRI3D:
        if (el.active) { st2d.unclosed++; element_end(); }
        rn_tri3d_intent(rec, len);
        break;
    case RN_INT_WORLDTRI:
        if (el.active) { st2d.unclosed++; element_end(); }
        rn_worldtri_intent(rec, len);
        break;
    case RN_INT_BILLBOARD:
        if (el.active) { st2d.unclosed++; element_end(); }
        rn_billboard_intent(rec, len);
        break;
    case RN_INT_CLAIM_END:
        element_end();
        rn_sky_end();
        rn_tri3d_end();
        rn_billboard_end();
        rn_worldtri_end();
        break;
    default:
        break;
    }
}

void rn_claims_init(void) {
    static int done;
    const char *e = getenv("PS2_RN_2D");
    if (done) return;
    done = 1;
    rn_sun_init();
    rn_sky_init();
    rn_tri3d_init();
    rn_billboard_init();
    rn_worldtri_init();
    rn_screen_init();
    {   const char *f = getenv("PS2_RN_FRONTEND");
        fe_on = !(f && *f == '0'); }
    on_2d = !(e && *e == '0');
    rn_claims_on = on_2d || rn_sun_on || rn_sky_on || rn_tri3d_on || rn_billboard_on
                || rn_worldtri_on || rn_screen_on || fe_on;
    if (on_2d)
        ps2_log("rn: native 2D layer on (PS2_RN_2D=0 turns it off)");
}

void rn_2d_report(void) {
    if (st_fe.prims)
        ps2_log("rn: front end -- %llu primitives: %llu native 2D, %llu native screen "
                "passes, %llu left emulated", (unsigned long long)st_fe.prims,
                (unsigned long long)st_fe.as2d, (unsigned long long)st_fe.screen,
                (unsigned long long)(st_fe.prims - st_fe.as2d - st_fe.screen));
    if (!st2d.elements) return;
    ps2_log("rn: 2D layer -- %llu elements (%llu accepted whole), %llu primitives: "
            "%llu accepted, %llu kept emulated; %llu elements not closed by their "
            "end record",
            (unsigned long long)st2d.elements, (unsigned long long)st2d.whole,
            (unsigned long long)st2d.prims,
            (unsigned long long)st2d.rejected[REJ_NONE],
            (unsigned long long)(st2d.prims - st2d.rejected[REJ_NONE]),
            (unsigned long long)st2d.unclosed);
    if (st2d.reanchored)
        ps2_log("rn:    %llu elements re-anchored for a centred widescreen HUD",
                (unsigned long long)st2d.reanchored);
    for (int i = 1; i < REJ_N; i++)
        if (st2d.rejected[i])
            ps2_log("rn:    kept emulated, %s: %llu", rej_name[i],
                    (unsigned long long)st2d.rejected[i]);
    if (st2d.to_screen)
        ps2_log("rn:    %llu of the primitives kept off the 2D layer drawn as native "
                "screen passes", (unsigned long long)st2d.to_screen);
    if (st2d.out_of_space)
        ps2_log("rn:    %llu elements cut short for held-primitive space; the rest "
                "of each was drawn emulated", (unsigned long long)st2d.out_of_space);
}
