#include "rn_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SITE_CHAIN 0x0011AAF0u
#define SITE_FLARE 0x00118BC8u
#define SITE_GHOST 0x001B0680u

enum { ROLE_UNKNOWN = 0xFF, ROLE_NONE = 0, ROLE_CHAIN, ROLE_FLARE, ROLE_GHOST };

static u8 roles[RN_EMIT_MAX];
static int roles_ready;

static int role_of(u32 e) {
    const rn_emitter *em;
    if (e < RN_EMIT_FIRST || e >= RN_EMIT_MAX) return ROLE_NONE;
    if (!roles_ready) {
        memset(roles, ROLE_UNKNOWN, sizeof roles);
        roles_ready = 1;
    }
    if (roles[e] != ROLE_UNKNOWN) return roles[e];
    em = rn_emitter_get(e);
    if (!em || !em->site) return ROLE_NONE;
    roles[e] = em->site == SITE_CHAIN ? ROLE_CHAIN
             : em->site == SITE_FLARE ? ROLE_FLARE
             : em->site == SITE_GHOST ? ROLE_GHOST : ROLE_NONE;
    return roles[e];
}

static struct {
    u64 frame;
    u32 slot;
    float box[4];
    u64 z_frame;
    float sun_z;
    u64 tmpl_frame;
    u32 tmpl_tbp;
} chain;

static struct {
    u64 frames_chain, frames_flare, frames_ghost, prims_claimed;
    u64 no_box, decode_fail;
} st_sun;

static u64 last_flare_frame, last_ghost_frame;

typedef struct { u64 frame; u32 tbp, cbp; u32 index; } dec_cache;
static dec_cache dcache[2];

static u32 fnv(const u8 *p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u32 decode(int which, u64 t0, u32 cbp_override) {
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 tbw = (u32)((t0 >> 14) & 0x3Full);
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    u32 tw = 1u << ((t0 >> 26) & 0xFull), th = 1u << ((t0 >> 30) & 0xFull);
    u32 cbp = cbp_override ? cbp_override : (u32)((t0 >> 37) & 0x3FFFull) * 256u;
    u32 cpsm = cbp_override ? 0u : (u32)((t0 >> 51) & 0xFull);
    dec_cache *dc = &dcache[which];
    static u8 buf[256 * 256 * 4];
    u32 index;
    if (dc->frame == (u64)ps2_gs_frame_count + 1u && dc->tbp == tbp && dc->cbp == cbp)
        return dc->index;
    if (tw > 256u || th > 256u
        || ps2_gs_decode_indexed(tbp, tbw, psm, tw, th, cbp, cpsm, buf) != 0) {
        st_sun.decode_fail++;
        return ~0u;
    }
    for (u32 i = 0; i < tw * th; i++) buf[i * 4u + 3u] = 255;
    index = ps2_vk_texture(0xF100000000000000ull | ((u64)which << 48)
                           | ((u64)(tbp >> 8) << 24) | (cbp >> 8),
                           fnv(buf, (size_t)tw * th * 4u), buf, tw, th);
    dc->frame = (u64)ps2_gs_frame_count + 1u;
    dc->tbp = tbp;
    dc->cbp = cbp;
    dc->index = index;
    return index;
}

static void observe_chain(const struct ps2_vk_state *st, int kind,
                          const struct ps2_vk_vertex *v) {
    u64 now = (u64)ps2_gs_frame_count + 1u;
    if (kind != RN_PRIM_SPRITE) return;
    if (st->tme && st->tex_rt && !st->tex_self && st->fst && !st->abe
        && chain.frame != now) {
        float s0 = v[0].s, s1 = v[2].s, t0 = v[0].t, t1 = v[2].t;
        chain.frame = now;
        chain.slot = st->tex_rt - 1u;
        chain.box[0] = s0 < s1 ? s0 : s1;
        chain.box[2] = s0 < s1 ? s1 : s0;
        chain.box[1] = t0 < t1 ? t0 : t1;
        chain.box[3] = t0 < t1 ? t1 : t0;
        st_sun.frames_chain++;
        return;
    }
    if (!st->tme && st->ztst == 2u && st->abe) {
        chain.z_frame = now;
        chain.sun_z = v[0].z;
        {   static long every = -1;
            if (every < 0) {
                const char *e = getenv("PS2_RN_SUN_LOG");
                every = e ? strtol(e, NULL, 0) : 0;
            }
            if (every > 0 && now % (u64)every == 0)
                ps2_log("rn-sun f%llu: box slot %u [%.1f,%.1f]..[%.1f,%.1f] "
                        "sun z %.9f zrt %u", (unsigned long long)now, chain.slot,
                        chain.box[0], chain.box[1], chain.box[2], chain.box[3],
                        chain.sun_z, st->zrt);
        }
        return;
    }
    if (st->tme && !st->tex_rt && st->abe && st->fst) {
        chain.tmpl_frame = now;
        chain.tmpl_tbp = (u32)(ps2_gs_cur_tex0() & 0x3FFFull) * 256u;
    }
}

int rn_sun_claim_prim(const struct ps2_vk_state *st, int kind,
                      const struct ps2_vk_vertex *v, int n) {
    int role = role_of(st->emitter);
    u64 now = (u64)ps2_gs_frame_count + 1u;
    ps2_vk_native nd;
    u64 t0;
    if (role == ROLE_NONE) return 0;
    if (role == ROLE_CHAIN) {
        observe_chain(st, kind, v);
        return 0;
    }
    if (!st->tme) return 0;
    if (role == ROLE_FLARE && kind != RN_PRIM_TRI) return 0;
    if (role == ROLE_GHOST && kind != RN_PRIM_SPRITE) return 0;
    t0 = ps2_gs_cur_tex0();
    memset(&nd, 0, sizeof nd);
    if (role == ROLE_FLARE) {
        nd.pass = PS2_VK_NATIVE_SUN_FLARE;
        nd.tex = decode(0, t0, 0u);
        if (last_flare_frame != now) { last_flare_frame = now; st_sun.frames_flare++; }
    } else {
        if (chain.tmpl_frame != now) return 0;
        nd.pass = PS2_VK_NATIVE_SUN_GHOST;
        nd.tex = decode(1, t0, chain.tmpl_tbp);
        if (last_ghost_frame != now) { last_ghost_frame = now; st_sun.frames_ghost++; }
    }
    if (nd.tex == ~0u) return 0;
    nd.rt = st->rt;
    nd.tex_w = st->tex_w;
    nd.tex_h = st->tex_h;
    nd.tex_point = st->tex_point;
    nd.wrap = st->wms | (st->wmt << 2);
    for (int i = 0; i < 4; i++) nd.scissor[i] = st->scissor[i];
    if (chain.frame == now && chain.z_frame == now) {
        nd.zrt = chain.slot;
        memcpy(nd.box, chain.box, sizeof nd.box);
        nd.sun_z = chain.sun_z;
    } else {
        nd.zrt = st->rt;
        st_sun.no_box++;
    }
    ps2_vk_native_draw(&nd, v, n);
    st_sun.prims_claimed++;
    return 1;
}

int rn_sun_on;
void rn_sun_init(void) {
    const char *e = getenv("PS2_RN_SUN");
    rn_sun_on = !(e && *e == '0');
    if (rn_sun_on)
        ps2_log("rn: native sun flare and lens ghosts on (PS2_RN_SUN=0 turns them "
                "off) -- the emulated draws of sub_118BC8 and sub_1B0680 are replaced");
}

void rn_sun_report(void) {
    if (!rn_sun_on) return;
    ps2_log("rn: native sun -- occlusion chain seen in %llu frames, flare drawn "
            "natively in %llu, ghosts in %llu; %llu primitives claimed; %llu "
            "without that frame's occlusion sample (the last visibility kept); "
            "%llu texture decodes refused",
            (unsigned long long)st_sun.frames_chain,
            (unsigned long long)st_sun.frames_flare,
            (unsigned long long)st_sun.frames_ghost,
            (unsigned long long)st_sun.prims_claimed,
            (unsigned long long)st_sun.no_box,
            (unsigned long long)st_sun.decode_fail);
}
