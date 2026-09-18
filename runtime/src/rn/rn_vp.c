#include "rn_vp_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_vp_mode;
int rn_vp_dirty = 1;

int vp_mesh_wanted(void) {
    static int sw = -1;
    if (sw < 0) {
        const char *e = getenv("PS2_RN_MESH");
        sw = !(e && *e == '0');
    }
    return sw && ps2_vk_mesh_available();
}

#define PKT_MAX 65536u
typedef struct { float x, y, z, w; } vp_xyzf;
static u32 pkt_lo[PKT_MAX][2], pkt_hi[PKT_MAX][2];
static vp_xyzf pkt_f[PKT_MAX];
static u8 pkt_isf[PKT_MAX];
static u32 pkt_n;
static int pkt_over;

#define KICK_MAX 1024u
static struct { u32 at, first, count; } kicks[KICK_MAX];
static u32 n_kicks, kick_next;

static void pkt_reset(void) { pkt_n = 0; pkt_over = 0; n_kicks = 0; kick_next = 0; }
u32 pkt_len(void) { return pkt_n; }

void pkt_kick(u32 at) {
    u32 first = n_kicks ? kicks[n_kicks - 1].first + kicks[n_kicks - 1].count : 0u;
    if (n_kicks >= KICK_MAX) { pkt_over = 1; return; }
    kicks[n_kicks].at = at & VP_QW_MASK;
    kicks[n_kicks].first = first;
    kicks[n_kicks].count = pkt_n - first;
    n_kicks++;
}

void pkt_put(u32 idx, const u32 w[4]) {
    if (idx >= PKT_MAX) { pkt_over = 1; return; }
    pkt_lo[idx][0] = w[0]; pkt_lo[idx][1] = w[1];
    pkt_hi[idx][0] = w[2]; pkt_hi[idx][1] = w[3];
    pkt_isf[idx] = 0;
    if (idx + 1u > pkt_n) pkt_n = idx + 1u;
}
void pkt_putf(u32 idx, const float *f) {
    u32 w[4];
    memcpy(w, f, 16);
    pkt_put(idx, w);
}
void pkt_put_xyzf(u32 idx, const float *pre, s32 wlane) {
    u32 w[4];
    w[0] = (u32)vp_ftoi(pre[0], 4);
    w[1] = (u32)vp_ftoi(pre[1], 4);
    w[2] = (u32)vp_ftoi(pre[2], 4);
    w[3] = (u32)wlane;
    pkt_put(idx, w);
    if (idx < PKT_MAX) {
        pkt_isf[idx] = 1;
        pkt_f[idx].x = pre[0]; pkt_f[idx].y = pre[1];
        pkt_f[idx].z = pre[2]; pkt_f[idx].w = pre[3];
    }
}

typedef struct {
    const char *name;
    u32 start, end;
    vp_fn fn;
    u16 *pcs;
    u32 *words;
    u32 n;
    int parsed;
    int off;
    u64 runs, declined;
    u64 declined_init;
} vp_prog;

static vp_prog progs[] = {
    { "model 39DB10",  0x0039DB10u, 0x0039FEA0u, rn_vp_model, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "part 3A0080",   0x003A0080u, 0x003A1100u, rn_vp_part, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "shadow 3A1110", 0x003A1110u, 0x003A1D30u, rn_vp_shadow, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "skinned 3A1D40", 0x003A1D40u, 0x003A40B0u, rn_vp_skin, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "aircraft 3A40C0", 0x003A40C0u, 0x003A6600u, rn_vp_aircraft, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "3A6610",        0x003A6610u, 0x003A91B0u, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "flat 3A9350",   0x003A9350u, 0x003AA3B0u, rn_vp_flat, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "3AA410",        0x003AA410u, 0x003ACE40u, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "3ACFE0",        0x003ACFE0u, 0x003AE100u, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "3AE110",        0x003AE110u, 0x003B0D50u, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "terrain 3B4080", 0x003B4080u, 0x003B7830u, rn_vp_terrain, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "detail 3BABD0", 0x003BABD0u, 0x003BC220u, rn_vp_detail, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "3BC600",        0x003BC600u, 0x003BD980u, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "loadmap 3BE3E0", 0x003BE3E0u, 0x003BEF00u, rn_vp_loadmap, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "lit 3BEF10",    0x003BEF10u, 0x003C0650u, rn_vp_lit, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "fx1 3C06F0",    0x003C06F0u, 0x003C2230u, rn_vp_fx1, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "rain 3C2240",   0x003C2240u, 0x003C3D70u, rn_vp_rain, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "fx2 3C3D80",    0x003C3D80u, 0x003C5210u, rn_vp_fx2, NULL, NULL, 0, 0, 0, 0, 0, 0 },
    { "trees 3C5220",  0x003C5220u, 0x003C6ED0u, rn_vp_trees, NULL, NULL, 0, 0, 0, 0, 0, 0 },
};
static u64 unrecognised_runs;
#define N_PROGS (sizeof progs / sizeof progs[0])

static int resident = -1;

int rn_vp_resident_slot(void) { return resident; }
const char *rn_vp_prog_name(int slot) {
    return slot >= 0 && (u32)slot < N_PROGS ? progs[slot].name : "unrecognised";
}

static void parse_prog(vp_prog *p) {
    u32 pos, n = 0, cap = 4096;
    p->parsed = 1;
    if (!ps2_ram) return;
    p->pcs = (u16 *)malloc(cap * sizeof *p->pcs);
    p->words = (u32 *)malloc(cap * 2 * sizeof *p->words);
    if (!p->pcs || !p->words) return;
    pos = p->start + 8u;
    while (pos + 4u <= p->end) {
        u32 code = ps2_r32(pos), cmd = (code >> 24) & 0x7Fu;
        pos += 4u;
        if (cmd == 0x4Au) {
            u32 k = ((code >> 16) & 0xFFu) ? ((code >> 16) & 0xFFu) : 256u;
            u32 addr = (code & 0xFFFFu) * 8u;
            for (u32 i = 0; i < k && n < cap; i++, pos += 8u) {
                p->pcs[n] = (u16)(addr + i * 8u);
                p->words[2 * n] = ps2_r32(pos);
                p->words[2 * n + 1] = ps2_r32(pos + 4u);
                n++;
            }
        } else if (cmd != 0x00u && cmd != 0x10u && cmd != 0x11u && cmd != 0x13u) {
            break;
        }
    }
    p->n = n;
    ps2_log("rn: native VU1 program %s: %u pairs", p->name, n);
}

static int prog_matches(vp_prog *p, const u8 *micro, u32 micro_size) {
    if (!p->parsed) parse_prog(p);
    if (!p->n) return 0;
    for (u32 i = 0; i < p->n; i++) {
        u32 lo, up;
        if (p->pcs[i] + 8u > micro_size) return 0;
        memcpy(&lo, micro + p->pcs[i], 4);
        memcpy(&up, micro + p->pcs[i] + 4, 4);
        if (lo != p->words[2 * i] || up != p->words[2 * i + 1]) return 0;
    }
    return 1;
}

static void pkt_submit_range(u32 i, u32 end) {
    u32 q_latch = 0;
    while (i < end) {
        u64 lo = (u64)pkt_lo[i][0] | ((u64)pkt_lo[i][1] << 32);
        u64 hi = (u64)pkt_hi[i][0] | ((u64)pkt_hi[i][1] << 32);
        u32 nloop = (u32)(lo & 0x7FFFu);
        u32 nreg = (u32)((lo >> 60) & 0xFu);
        u32 flg = (u32)((lo >> 58) & 3u);
        int eop = (int)((lo >> 15) & 1u);
        if (!nreg) nreg = 16u;
        i++;
        if (flg != 0u) {
            ps2_log("rn: native packet with GIFtag FLG %u; not submitted", flg);
            return;
        }
        if ((lo >> 46) & 1u) ps2_gs_write_reg(0x00, (lo >> 47) & 0x7FFu);
        for (u32 l = 0; l < nloop; l++) {
            for (u32 r = 0; r < nreg; r++, i++) {
                u32 reg = (u32)((hi >> (r * 4u)) & 0xFu);
                const u32 *a, *b;
                if (i >= end) return;
                a = pkt_lo[i];
                b = pkt_hi[i];
                switch (reg) {
                case 0x0: ps2_gs_write_reg(0x00, a[0] & 0x7FFu); break;
                case 0x1:
                    ps2_gs_write_reg(0x01, (u64)(a[0] & 0xFFu) | ((u64)(a[1] & 0xFFu) << 8)
                                         | ((u64)(b[0] & 0xFFu) << 16)
                                         | ((u64)(b[1] & 0xFFu) << 24)
                                         | ((u64)q_latch << 32));
                    break;
                case 0x2:
                    q_latch = b[0];
                    ps2_gs_write_reg(0x02, (u64)a[0] | ((u64)a[1] << 32));
                    break;
                case 0x3:
                    ps2_gs_write_reg(0x03, (u64)(a[0] & 0x3FFFu) | ((u64)(a[1] & 0x3FFFu) << 16));
                    break;
                case 0x4: case 0x5: {
                    int adc = (b[1] & 0x8000u) != 0;
                    if (pkt_isf[i]) {
                        const vp_xyzf *f = &pkt_f[i];
                        ps2_gs_native_vertex(f->x, f->y, (double)f->z,
                                             (float)((b[1] >> 4) & 0xFFu),
                                             reg == 0x4 ? 0 : 1, adc);
                    } else if (reg == 0x4) {
                        ps2_gs_write_reg(adc ? 0x0Cu : 0x04u,
                            (u64)(a[0] & 0xFFFFu) | ((u64)(a[1] & 0xFFFFu) << 16)
                          | ((u64)((b[0] >> 4) & 0xFFFFFFu) << 32)
                          | ((u64)((b[1] >> 4) & 0xFFu) << 56));
                    } else {
                        ps2_gs_write_reg(adc ? 0x0Du : 0x05u,
                            (u64)(a[0] & 0xFFFFu) | ((u64)(a[1] & 0xFFFFu) << 16)
                          | ((u64)b[0] << 32));
                    }
                    break;
                }
                case 0xA: ps2_gs_write_reg(0x0A, (u64)((b[1] >> 4) & 0xFFu) << 56); break;
                case 0xE: ps2_gs_write_reg(b[0] & 0xFFu, (u64)a[0] | ((u64)a[1] << 32)); break;
                case 0xF: break;
                default: ps2_gs_write_reg(reg, (u64)a[0] | ((u64)a[1] << 32)); break;
                }
            }
        }
        if (eop) break;
    }
}

static void pkt_submit(void) {
    rn_census_native_prog = 1;
    if (!n_kicks) pkt_submit_range(0, pkt_n);
    else
        for (u32 k = 0; k < n_kicks; k++)
            pkt_submit_range(kicks[k].first, kicks[k].first + kicks[k].count);
    rn_census_native_prog = 0;
}

void pkt_flush(void) {
    if (pkt_n && !pkt_over) pkt_submit();
    pkt_reset();
}

static struct {
    u32 pending;
    u64 compared, matched, qw_mismatch, elsewhere, no_kick;
    int shown;
} ver;

static u8 verify_mem[PS2_VU1_MEM_SIZE];

void rn_vp_kick(ps2_vu *vu, u32 addr) {
    u32 a = addr & VP_QW_MASK, bad = 0, first = ~0u, i0, n;
    if (!ver.pending) return;
    ver.pending--;
    ver.compared++;
    i0 = kicks[kick_next].first;
    n = kicks[kick_next].count;
    if (a != kicks[kick_next].at) {
        kick_next++;
        ver.elsewhere++;
        return;
    }
    kick_next++;
    for (u32 i = 0; i < n; i++) {
        u32 w[4];
        memcpy(w, vu->mem + (((a + i) & VP_QW_MASK) * 16u), 16);
        if (w[0] != pkt_lo[i0 + i][0] || w[1] != pkt_lo[i0 + i][1]
            || w[2] != pkt_hi[i0 + i][0] || w[3] != pkt_hi[i0 + i][1]) {
            if (first == ~0u) first = i;
            bad++;
        }
    }
    if (!bad) { ver.matched++; return; }
    ver.qw_mismatch++;
    if (ver.shown < 16) {
        u32 w[4];
        ver.shown++;
        memcpy(w, vu->mem + (((a + first) & VP_QW_MASK) * 16u), 16);
        ps2_log("rn-vp: %s packet at %03X differs in %u of %u qw; first at +%u: "
                "vu %08X %08X %08X %08X native %08X %08X %08X %08X",
                rn_vp_prog_name(rn_vp_resident_slot()), a, bad, n,
                first, w[0], w[1], w[2], w[3], pkt_lo[i0 + first][0], pkt_lo[i0 + first][1],
                pkt_hi[i0 + first][0], pkt_hi[i0 + first][1]);
    }
}

void rn_vp_init(void) {
    const char *e = getenv("PS2_RN_VP");
    if (e && *e == '0') {
        ps2_log("rn: native VU1 programs off (PS2_RN_VP=0)");
        return;
    }
    rn_vp_mode = (e && !strcmp(e, "verify")) ? 2 : 1;
    for (u32 i = 0; i < N_PROGS; i++) {
        char key[48] = "PS2_RN_VP_";
        u32 k = 10;
        const char *n = progs[i].name;
        for (; *n && *n != ' ' && k + 1 < sizeof key; n++)
            key[k++] = (char)(*n >= 'a' && *n <= 'z' ? *n - 32 : *n);
        key[k] = 0;
        {   const char *v = getenv(key);
            progs[i].off = v && *v == '0';
            if (progs[i].off) ps2_log("rn: native VU1 %s off (%s=0)", progs[i].name, key);
        }
    }
    ps2_log("rn: native VU1 programs on (%s)",
            rn_vp_mode == 2 ? "verifying against VU1" : "native");
}

int rn_vp_run(ps2_vu *vu, u32 start) {
    vpx x;
    vp_prog *p;
    u32 kick_at = 0;
    int r;
    if (rn_vp_dirty) {
        rn_vp_dirty = 0;
        resident = -1;
        for (u32 i = 0; i < N_PROGS; i++)
            if (prog_matches(&progs[i], vu->micro, vu->micro_size)) { resident = (int)i; break; }
    }
    if (resident < 0) { unrecognised_runs++; return 0; }
    p = &progs[resident];
    if (!p->fn || p->off) {
        if (!p->declined) {
            ps2_log("rn: VU1 program %s first runs at field %u", p->name,
                    ps2_gs_frame_count);
        }
        p->declined++;
        if ((start & (vu->micro_size - 8u)) == 0u) p->declined_init++;
        return 0;
    }
    memcpy(x.vf, vu->vf, sizeof x.vf);
    memcpy(x.vi, vu->vi, sizeof x.vi);
    x.top = vu->top;
    x.tpc = vu->tpc;
    x.clip = vu->clip;
    x.r = vu->r;
    x.q = vu->q;
    x.p = vu->p;
    x.native = rn_vp_mode == 1;
    if (rn_vp_mode == 2) {
        memcpy(verify_mem, vu->mem, vu->mem_size < sizeof verify_mem
                                    ? vu->mem_size : sizeof verify_mem);
        x.mem = verify_mem;
    } else {
        x.mem = vu->mem;
    }
    ver.no_kick += ver.pending;
    ver.pending = 0;
    pkt_reset();
    r = p->fn(&x, start & (vu->micro_size - 8u), &kick_at);
    if (r < 0) {
        p->declined++;
        if ((start & (vu->micro_size - 8u)) == 0u) p->declined_init++;
        return 0;
    }
    p->runs++;
    if (rn_vp_mode == 2) {
        if (pkt_over) return 0;
        if (r > 0 && pkt_n && !n_kicks) pkt_kick(kick_at);
        ver.pending = n_kicks;
        kick_next = 0;
        return 0;
    }
    memcpy(vu->vf, x.vf, sizeof x.vf);
    memcpy(vu->vi, x.vi, sizeof x.vi);
    vu->tpc = x.tpc;
    vu->clip = x.clip;
    vu->r = x.r;
    vu->q = x.q;
    vu->p = x.p;
    if (r > 0 && pkt_n && !pkt_over) pkt_submit();
    return 1;
}

int rn_vp_upload(ps2_vu *vu, u32 start) {
    for (u32 i = 0; i < N_PROGS; i++) {
        vp_prog *p = &progs[i];
        if (p->start != start) continue;
        if (!p->parsed) parse_prog(p);
        if (!p->n) return -1;
        for (u32 k = 0; k < p->n; k++) {
            if (p->pcs[k] + 8u > vu->micro_size) return -1;
            memcpy(vu->micro + p->pcs[k], &p->words[2 * k], 4);
            memcpy(vu->micro + p->pcs[k] + 4, &p->words[2 * k + 1], 4);
        }
        rn_vp_dirty = 1;
        return (int)i;
    }
    return -1;
}

void rn_vp_verify_stats(int prog, u64 out[7]) {
    out[0] = ver.compared; out[1] = ver.matched; out[2] = ver.qw_mismatch;
    out[3] = ver.elsewhere; out[4] = ver.no_kick + ver.pending;
    out[5] = prog >= 0 && (u32)prog < N_PROGS ? progs[prog].runs : 0;
    out[6] = prog >= 0 && (u32)prog < N_PROGS ? progs[prog].declined : 0;
}

void rn_vp_report(void) {
    if (!rn_vp_mode) return;
    for (u32 i = 0; i < N_PROGS; i++)
        if (progs[i].runs || progs[i].declined)
            ps2_log("rn: native VU1 %-13s -- %llu activations native, %llu left to VU1"
                    " (%llu of them the init entry)%s",
                    progs[i].name, (unsigned long long)progs[i].runs,
                    (unsigned long long)progs[i].declined,
                    (unsigned long long)progs[i].declined_init,
                    progs[i].fn ? "" : " (not written yet)");
    if (unrecognised_runs)
        ps2_log("rn: native VU1 -- %llu activations of an unrecognised image",
                (unsigned long long)unrecognised_runs);
    rn_vp_model_report();
    rn_vp_terrain_report();
    rn_vp_shadow_report();
    rn_vp_sprite_report();
    rn_vp_load_report();
    rn_vp_lit_report();
    rn_vp_fx_report();
    rn_vp_flat_report();
    ps2_gs_native_tex_report();
    if (rn_vp_mode == 2)
        ps2_log("rn: native VU1 verify -- %llu packets compared, %llu identical, "
                "%llu differ, %llu kicked elsewhere, %llu never kicked",
                (unsigned long long)ver.compared, (unsigned long long)ver.matched,
                (unsigned long long)ver.qw_mismatch, (unsigned long long)ver.elsewhere,
                (unsigned long long)ver.no_kick);
}
