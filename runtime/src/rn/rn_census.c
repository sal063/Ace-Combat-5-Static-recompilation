#include "rn_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_census_on;
u32 rn_gs_emitter;

static rn_emitter emitters[RN_EMIT_MAX];
static u32 n_emitters = RN_EMIT_FIRST;

u32 rn_emitter_count(void) { return n_emitters; }

const rn_emitter *rn_emitter_get(u32 id) {
    return id < n_emitters ? &emitters[id] : NULL;
}

void rn_emitter_define(u32 id, u32 site, u32 func, u32 via) {
    if (id < RN_EMIT_FIRST || id >= RN_EMIT_MAX) return;
    emitters[id].site = site;
    emitters[id].func = func;
    emitters[id].via = via;
    if (id >= n_emitters) n_emitters = id + 1u;
}

rn_emitter *rn_emitter_mut(u32 id) {
    return id >= RN_EMIT_FIRST && id < n_emitters ? &emitters[id] : NULL;
}

const char *rn_emitter_name(u32 id, char *buf, size_t cap) {
    static const char *reserved[RN_EMIT_FIRST] = {
        "(none)", "DrawCtrl::flush (display, clears)", "(ordering-table heads)",
        "(DMA kick outside the frame)", "(packet buffer, unowned)",
        "(reserved 5)", "(reserved 6)", "(reserved 7)" };
    if (id < RN_EMIT_FIRST) {
        snprintf(buf, cap, "%s", reserved[id]);
        return buf;
    }
    if (id >= n_emitters) {
        snprintf(buf, cap, "(emitter %u)", id);
        return buf;
    }
    {
        const rn_emitter *e = &emitters[id];
        const char *nm = e->func ? ps2_symbol_name(e->func) : NULL;
        static const char *via[4] = { "", " via 2D", " direct", "" };
        const char *v = via[e->via < 4 ? e->via : 0];
        if (e->via == RN_VIA_WRITER && !nm)
            snprintf(buf, cap, "sub_%08X writer", e->site);
        else if (e->via == RN_VIA_WRITER)
            snprintf(buf, cap, "%s (writer)", nm);
        else if (nm)
            snprintf(buf, cap, "%s+0x%X%s", nm, e->site - e->func, v);
        else if (e->func)
            snprintf(buf, cap, "sub_%08X+0x%X%s", e->func, e->site - e->func, v);
        else
            snprintf(buf, cap, "site_%08X%s", e->site, v);
    }
    return buf;
}

static double total_native_px;
int rn_census_vu_prog;
int rn_census_native_prog;
int rn_vu_kicking;
static double vu_prog_px[64];

#define CEN_SLOTS 8192u
typedef struct {
    u32 key;
    u32 rt_mask;
    u64 frames, last_frame, first_frame;
    u64 prims[4];
    double pixels;
    double pixels_rt0;
    double pixels_native;
    double frame_px;
    double peak_frame_px;
} cen_entry;

static cen_entry cen[CEN_SLOTS];
static u32 cen_used;
static int cen_full_said;
static u32 cur_scene;
static u64 cen_frame;

#define SCENE_SLOTS 256u
typedef struct {
    u32 key;
    u64 frames;
    double px, px_attrib;
    double worst_frac;
    u64 worst_frame;
} scene_entry;
static scene_entry scenes[SCENE_SLOTS];
static double frame_px, frame_px_attrib;

static u32 touched[CEN_SLOTS];
static u32 n_touched;

static void cen_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    for (u32 i = 0; i < CEN_SLOTS; i++) cen[i].key = 0xFFFFFFFFu;
}

static cen_entry *cen_slot(u32 scene, u32 emitter) {
    u32 key = ((scene & 0xFFFFu) << 16) | (emitter & 0xFFFFu);
    u32 h = (key * 2654435761u) >> 19;
    cen_init_once();
    for (u32 probe = 0; probe < CEN_SLOTS; probe++) {
        cen_entry *c = &cen[(h + probe) & (CEN_SLOTS - 1u)];
        if (c->key == key) return c;
        if (c->key == 0xFFFFFFFFu) {
            if (cen_used >= CEN_SLOTS - 64u) break;
            c->key = key;
            c->first_frame = cen_frame;
            c->last_frame = ~0ull;
            cen_used++;
            return c;
        }
    }
    if (!cen_full_said) {
        cen_full_said = 1;
        ps2_log("rn: census table full (%u scene/emitter pairs); later pairs "
                "are not counted", cen_used);
    }
    return NULL;
}

static scene_entry *scene_slot(u32 scene) {
    u32 h = (scene * 40503u) & (SCENE_SLOTS - 1u);
    for (u32 probe = 0; probe < SCENE_SLOTS; probe++) {
        scene_entry *s = &scenes[(h + probe) & (SCENE_SLOTS - 1u)];
        if (s->key == scene + 1u) return s;
        if (!s->key) {
            s->key = scene + 1u;
            s->worst_frac = 2.0;
            return s;
        }
    }
    return NULL;
}

void rn_gs_tag(u32 kind, u32 a, u32 b) {
    if (PS2_UNLIKELY(rn_dump_on)) rn_dump_tag(kind, a, b);
    switch (kind) {
    case RN_TAG_EMITTER:
        rn_gs_emitter = a < RN_EMIT_MAX ? a : RN_EMIT_NONE;
        break;
    case RN_TAG_SCENE:
        cur_scene = a & 0xFFFFu;
        break;
    case RN_TAG_DEFINE: {
        u32 id = a & 0xFFFFu;
        if (id >= RN_EMIT_FIRST && id < RN_EMIT_MAX && emitters[id].site != b)
            rn_emitter_define(id, b, (a >> 16) == RN_VIA_WRITER ? b : 0u,
                              a >> 16);
        break;
    }
    default:
        break;
    }
    if (!rn_census_on) {
        static int asked;
        if (!asked) {
            const char *e = getenv("PS2_RN_CENSUS");
            asked = 1;
            if (!e || *e != '0') rn_census_on = 1;
        }
    }
}

static double clip_len(double a0, double a1, double lo, double hi) {
    if (a0 > a1) { double t = a0; a0 = a1; a1 = t; }
    if (a0 < lo) a0 = lo;
    if (a1 > hi) a1 = hi;
    return a1 > a0 ? a1 - a0 : 0.0;
}

void rn_census_prim_slow(const struct ps2_vk_state *st, int kind,
                         const struct ps2_vk_vertex *v, int n, int native) {
    double px = 0.0;
    double sx0 = (double)st->scissor[0], sx1 = (double)st->scissor[1] + 1.0;
    double sy0 = (double)st->scissor[2], sy1 = (double)st->scissor[3] + 1.0;
    u32 e = rn_gs_emitter < RN_EMIT_MAX ? rn_gs_emitter : RN_EMIT_NONE;
    cen_entry *c;
    if (rn_census_native_prog) native = 1;
    switch (kind) {
    case RN_PRIM_SPRITE:
        if (n >= 3)
            px = clip_len(v[0].x, v[2].x, sx0, sx1)
               * clip_len(v[0].y, v[2].y, sy0, sy1);
        break;
    case RN_PRIM_TRI:
        if (n >= 3) {
            double ax = v[1].x - v[0].x, ay = v[1].y - v[0].y;
            double bx = v[2].x - v[0].x, by = v[2].y - v[0].y;
            double area = fabs(ax * by - ay * bx) * 0.5;
            double x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
            for (int i = 1; i < 3; i++) {
                if (v[i].x < x0) x0 = v[i].x;
                if (v[i].x > x1) x1 = v[i].x;
                if (v[i].y < y0) y0 = v[i].y;
                if (v[i].y > y1) y1 = v[i].y;
            }
            {
                double bw = x1 - x0, bh = y1 - y0;
                double cw = clip_len(x0, x1, sx0, sx1);
                double ch = clip_len(y0, y1, sy0, sy1);
                if (bw > 0.0 && bh > 0.0) area *= (cw / bw) * (ch / bh);
                else if (cw <= 0.0 || ch <= 0.0) area = 0.0;
            }
            px = area;
        }
        break;
    case RN_PRIM_LINE:
        if (n >= 2) {
            double dx = clip_len(v[0].x, v[1].x, sx0, sx1);
            double dy = clip_len(v[0].y, v[1].y, sy0, sy1);
            px = (dx > dy ? dx : dy) + 1.0;
        }
        break;
    default:
        px = 1.0;
        break;
    }
    {
        static int init;
        static u32 site, cbp, em_id;
        static u64 every = 600;
        if (!init) {
            const char *s = getenv("PS2_RN_TRACE_SITE");
            const char *ei = getenv("PS2_RN_TRACE_EMITTER");
            em_id = ei ? (u32)strtoul(ei, NULL, 0) : 0u;
            const char *n = getenv("PS2_RN_TRACE_EVERY");
            const char *c = getenv("PS2_RN_TRACE_CBP");
            init = 1;
            site = s ? (u32)strtoul(s, NULL, 16) : 0u;
            cbp = c ? (u32)strtoul(c, NULL, 0) : 0u;
            if (n && strtoull(n, NULL, 0)) every = strtoull(n, NULL, 0);
        }
        if (((site && e >= RN_EMIT_FIRST && e < n_emitters
              && emitters[e].site == site)
             || (em_id && e == em_id)
             || (cbp && st->tme
                 && (u32)((ps2_gs_cur_tex0() >> 37) & 0x3FFFull) * 256u == cbp))
            && cen_frame % every == 0) {
            u64 t0 = ps2_gs_cur_tex0(), fr = ps2_gs_cur_frame();
            u64 zb = ps2_gs_cur_zbuf();
            char vb[512];
            int o = 0;
            for (int i = 0; i < n && i < 4 && o < 440; i++)
                o += snprintf(vb + o, sizeof vb - (size_t)o,
                              " (%.1f,%.1f z%.6f rgba %.0f,%.0f,%.0f,%.0f "
                              "stq %g,%g,%g)", v[i].x, v[i].y, v[i].z,
                              v[i].r, v[i].g, v[i].b, v[i].a, v[i].s, v[i].t,
                              v[i].q);
            ps2_log("rn-trace f%llu em%u k%d rt%u fbp=%u psm=%u fbmsk=%08X "
                    "zbp=%u zpsm=%u zmsk=%u | tme=%d tbp=%u tbw=%u tpsm=%u "
                    "tw=%u th=%u tcc=%u tfx=%u cbp=%u cpsm=%u csm=%u texrt=%u "
                    "texdepth=%d self=%d fst=%d | abe=%d blend=(%u-%u)*%u+%u "
                    "fix=%u | ate=%d atst=%u aref=%u afail=%u zte=%d ztst=%u "
                    "zw=%d date=%d datm=%d sc=%d..%d,%d..%d |%s",
                    (unsigned long long)cen_frame, e, kind, st->rt,
                    (u32)(fr & 0x1FFull) * 8192u, (u32)((fr >> 24) & 0x3Full),
                    (u32)(fr >> 32), (u32)(zb & 0x1FFull) * 8192u,
                    (u32)((zb >> 24) & 0xFull), (u32)((zb >> 32) & 1ull),
                    st->tme, (u32)(t0 & 0x3FFFull) * 256u,
                    (u32)((t0 >> 14) & 0x3Full), (u32)((t0 >> 20) & 0x3Full),
                    1u << ((t0 >> 26) & 0xFull), 1u << ((t0 >> 30) & 0xFull),
                    (u32)((t0 >> 34) & 1ull), (u32)((t0 >> 35) & 3ull),
                    (u32)((t0 >> 37) & 0x3FFFull) * 256u,
                    (u32)((t0 >> 51) & 0xFull), (u32)((t0 >> 55) & 1ull),
                    st->tex_rt, st->tex_depth, st->tex_self, st->fst, st->abe,
                    st->alpha_a, st->alpha_b, st->alpha_c, st->alpha_d,
                    st->alpha_fix, st->ate, st->atst, st->aref, st->afail,
                    st->zte, st->ztst, st->zwrite, st->date, st->datm,
                    st->scissor[0], st->scissor[1], st->scissor[2],
                    st->scissor[3], vb);
        }
    }
    {
        static int init, on;
        static u8 seen[RN_EMIT_MAX];
        if (!init) {
            const char *s = getenv("PS2_RN_TRACE_TEXRT");
            init = 1;
            on = s && *s && *s != '0';
        }
        if (on && st->tex_rt && e < RN_EMIT_MAX && !seen[e] && n > 0) {
            seen[e] = 1;
            ps2_log("rn-texrt f%llu em%u site %08X via %u rt%u reads slot %u k%d "
                    "(%.1f,%.1f)..(%.1f,%.1f)", (unsigned long long)cen_frame, e,
                    e < n_emitters ? emitters[e].site : 0u,
                    e < n_emitters ? emitters[e].via : 0u, st->rt,
                    st->tex_rt - 1u, kind, v[0].x, v[0].y, v[n - 1].x, v[n - 1].y);
        }
    }
    c = cen_slot(cur_scene, e);
    if (!c) return;
    if (c->last_frame != cen_frame) {
        c->last_frame = cen_frame;
        c->frames++;
        c->frame_px = 0.0;
        if (n_touched < CEN_SLOTS) touched[n_touched++] = (u32)(c - cen);
    }
    c->prims[kind & 3]++;
    c->pixels += px;
    c->frame_px += px;
    if (st->rt == 0u) c->pixels_rt0 += px;
    if (native) {
        c->pixels_native += px;
        total_native_px += px;
    } else if (rn_census_vu_prog > 0 && rn_census_vu_prog < 64) {
        vu_prog_px[rn_census_vu_prog] += px;
    }
    if (st->rt < 32u) c->rt_mask |= 1u << st->rt;
    frame_px += px;
    if (e >= RN_EMIT_FIRST || e == RN_EMIT_DRAWCTRL) frame_px_attrib += px;
}

void rn_census_frame(void) {
    if (!rn_census_on) return;
    for (u32 i = 0; i < n_touched; i++) {
        cen_entry *c = &cen[touched[i]];
        if (c->frame_px > c->peak_frame_px) c->peak_frame_px = c->frame_px;
    }
    n_touched = 0;
    if (frame_px > 0.0) {
        scene_entry *s = scene_slot(cur_scene);
        if (s) {
            s->frames++;
            s->px += frame_px;
            s->px_attrib += frame_px_attrib;
            if (frame_px > 1000.0) {
                double f = frame_px_attrib / frame_px;
                if (f < s->worst_frac) {
                    s->worst_frac = f;
                    s->worst_frame = cen_frame;
                }
            }
        }
    }
    frame_px = frame_px_attrib = 0.0;
    cen_frame++;
}

typedef struct { u32 idx; double px; } cen_sort;
static int by_pixels(const void *a, const void *b) {
    double x = ((const cen_sort *)a)->px, y = ((const cen_sort *)b)->px;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void rt_list(u32 mask, char *buf, size_t cap) {
    int o = 0;
    buf[0] = 0;
    for (u32 i = 0; i < 32u && o < (int)cap - 5; i++)
        if (mask & (1u << i))
            o += snprintf(buf + o, cap - (size_t)o, "%s%u", o ? "," : "", i);
}

static void write_csv(const char *path) {
    FILE *fp = fopen(path, "w");
    char nm[160], rts[128];
    if (!fp) {
        ps2_log("rn: cannot write the render map to %s", path);
        return;
    }
    fprintf(fp, "scene,emitter,site,func,via,name,frames,first_frame,"
                "last_frame,points,lines,tris,sprites,pixels,pixels_rt0,"
                "pixels_per_frame,peak_frame_pixels,rt_slots,pixels_native\n");
    for (u32 i = 0; i < CEN_SLOTS; i++) {
        const cen_entry *c = &cen[i];
        const rn_emitter *e;
        u32 id;
        if (c->key == 0xFFFFFFFFu) continue;
        id = c->key & 0xFFFFu;
        e = rn_emitter_get(id);
        rn_emitter_name(id, nm, sizeof nm);
        for (char *p = nm; *p; p++) if (*p == ',') *p = ';';
        rt_list(c->rt_mask, rts, sizeof rts);
        fprintf(fp, "%u.%u,%u,%08X,%08X,%u,%s,%llu,%llu,%llu,%llu,%llu,%llu,"
                    "%llu,%.0f,%.0f,%.1f,%.0f,\"%s\",%.0f\n",
                (c->key >> 24) & 0xFFu, (c->key >> 16) & 0xFFu, id,
                e && id >= RN_EMIT_FIRST ? e->site : 0u,
                e && id >= RN_EMIT_FIRST ? e->func : 0u,
                e && id >= RN_EMIT_FIRST ? e->via : 0u, nm,
                (unsigned long long)c->frames,
                (unsigned long long)c->first_frame,
                (unsigned long long)c->last_frame,
                (unsigned long long)c->prims[0], (unsigned long long)c->prims[1],
                (unsigned long long)c->prims[2], (unsigned long long)c->prims[3],
                c->pixels, c->pixels_rt0,
                c->frames ? c->pixels / (double)c->frames : 0.0,
                c->peak_frame_px, rts, c->pixels_native);
    }
    fclose(fp);
    ps2_log("rn: render map written to %s (%u scene/emitter rows)", path,
            cen_used);
}

static void print_table(u32 scene_filter, unsigned top) {
    cen_sort *order = (cen_sort *)malloc(sizeof *order * CEN_SLOTS);
    u32 n = 0;
    double total = 0.0;
    char nm[160], rts[128];
    if (!order) return;
    for (u32 i = 0; i < CEN_SLOTS; i++) {
        if (cen[i].key == 0xFFFFFFFFu) continue;
        if (scene_filter != ~0u && (cen[i].key >> 16) != scene_filter) continue;
        order[n].idx = i;
        order[n].px = cen[i].pixels;
        total += cen[i].pixels;
        n++;
    }
    qsort(order, n, sizeof *order, by_pixels);
    for (u32 k = 0; k < n && k < top; k++) {
        const cen_entry *c = &cen[order[k].idx];
        u32 id = c->key & 0xFFFFu;
        rt_list(c->rt_mask, rts, sizeof rts);
        ps2_log("   %5.1f%%  scene %u.%-2u  #%-4u %-44s  %8.0f px/frame over "
                "%-6llu frames  native %3.0f%%  prims t%llu s%llu l%llu p%llu  rt %s",
                total > 0.0 ? 100.0 * c->pixels / total : 0.0,
                (c->key >> 24) & 0xFFu, (c->key >> 16) & 0xFFu, id,
                rn_emitter_name(id, nm, sizeof nm),
                c->frames ? c->pixels / (double)c->frames : 0.0,
                (unsigned long long)c->frames,
                c->pixels > 0.0 ? 100.0 * c->pixels_native / c->pixels : 0.0,
                (unsigned long long)c->prims[2], (unsigned long long)c->prims[3],
                (unsigned long long)c->prims[1], (unsigned long long)c->prims[0],
                rts);
    }
    if (n > top)
        ps2_log("   ... %u more rows (PS2_RENDER_MAP=<file> writes them all)",
                n - top);
    free(order);
}

static void print_scenes(void) {
    double px = 0.0, att = 0.0;
    for (u32 i = 0; i < SCENE_SLOTS; i++) {
        const scene_entry *s = &scenes[i];
        if (!s->key || s->px <= 0.0) continue;
        px += s->px;
        att += s->px_attrib;
        ps2_log("   scene %u.%-2u %7llu frames  %12.0f px  %6.2f%% attributed"
                "  worst frame %.2f%% (#%llu)",
                ((s->key - 1u) >> 8) & 0xFFu, (s->key - 1u) & 0xFFu,
                (unsigned long long)s->frames, s->px,
                100.0 * s->px_attrib / s->px,
                s->worst_frac <= 1.0 ? 100.0 * s->worst_frac : 100.0,
                (unsigned long long)s->worst_frame);
    }
    for (int i = 1; i < 64; i++)
        if (vu_prog_px[i] > 0.0 && px > 0.0)
            ps2_log("   emulated by VU1 program %-16s %14.0f px  %5.2f%%",
                    rn_vp_prog_name(i - 1), vu_prog_px[i], 100.0 * vu_prog_px[i] / px);
    if (px > 0.0)
        ps2_log("   all scenes: %.0f px, %.2f%% attributed to a named emitter; "
                "%.2f%% of GS primitive pixels claimed by native passes (native "
                "VU1 meshes are not GS primitives and are not in this table)",
                px, 100.0 * att / px, 100.0 * total_native_px / px);
}

void rn_census_report(const char *why) {
    if (!rn_census_on) return;
    ps2_log("---- render map (%s): %u emitters, %llu frames ----",
            why ? why : "", rn_emitter_count() - RN_EMIT_FIRST,
            (unsigned long long)cen_frame);
    print_scenes();
    print_table(cur_scene, 40);
}

void rn_report(void) {
    const char *path = getenv("PS2_RENDER_MAP");
    rn_sun_report();
    rn_sky_report();
    rn_tri3d_report();
    rn_billboard_report();
    rn_worldtri_report();
    rn_2d_report();
    rn_screen_report();
    rn_vp_report();
    if (!rn_census_on) return;
    ps2_log("---- render map: %u emitters, %llu frames ----",
            rn_emitter_count() - RN_EMIT_FIRST, (unsigned long long)cen_frame);
    print_scenes();
    print_table(~0u, 60);
    if (path && *path) write_csv(path);
}
