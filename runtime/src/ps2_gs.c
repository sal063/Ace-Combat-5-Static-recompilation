#include "ps2_runtime.h"
extern int ps2_gif_path;
#include "ps2_vk.h"
#include "ps2_settings.h"
#include "ps2_hle.h"
#include "ps2_statecap.h"
#include "ps2_capture.h"
#include "rn.h"
#include "rn/rn_int.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>

u64  ps2_gs_priv_read(u32 addr);
void ps2_gs_priv_write(u32 addr, u64 val);
void ps2_gfxq_drain(void);
int  ps2_gfxq_field(void);
extern int ps2_gfxq_on;
u32  ps2_gs_display_width(void);
u32  ps2_gs_display_height(void);
static u64 display_reg(void);
static u64 dispfb_reg(void);

#define GS_VRAM_SIZE (4 * 1024 * 1024)

enum {
    GS_PRIM = 0x00, GS_RGBAQ = 0x01, GS_ST = 0x02, GS_UV = 0x03,
    GS_XYZF2 = 0x04, GS_XYZ2 = 0x05, GS_TEX0_1 = 0x06, GS_TEX0_2 = 0x07,
    GS_CLAMP_1 = 0x08, GS_CLAMP_2 = 0x09, GS_FOG = 0x0A,
    GS_XYZF3 = 0x0C, GS_XYZ3 = 0x0D,
    GS_TEX1_1 = 0x14, GS_TEX1_2 = 0x15, GS_TEX2_1 = 0x16, GS_TEX2_2 = 0x17,
    GS_XYOFFSET_1 = 0x18, GS_XYOFFSET_2 = 0x19,
    GS_PRMODECONT = 0x1A, GS_PRMODE = 0x1B, GS_TEXCLUT = 0x1C,
    GS_SCANMSK = 0x22,
    GS_MIPTBP1_1 = 0x34, GS_MIPTBP1_2 = 0x35,
    GS_MIPTBP2_1 = 0x36, GS_MIPTBP2_2 = 0x37,
    GS_TEXA = 0x3B, GS_FOGCOL = 0x3D, GS_TEXFLUSH = 0x3F,
    GS_SCISSOR_1 = 0x40, GS_SCISSOR_2 = 0x41,
    GS_ALPHA_1 = 0x42, GS_ALPHA_2 = 0x43,
    GS_DIMX = 0x44, GS_DTHE = 0x45, GS_COLCLAMP = 0x46,
    GS_TEST_1 = 0x47, GS_TEST_2 = 0x48, GS_PABE = 0x49,
    GS_FBA_1 = 0x4A, GS_FBA_2 = 0x4B,
    GS_FRAME_1 = 0x4C, GS_FRAME_2 = 0x4D,
    GS_ZBUF_1 = 0x4E, GS_ZBUF_2 = 0x4F,
    GS_BITBLTBUF = 0x50, GS_TRXPOS = 0x51, GS_TRXREG = 0x52,
    GS_TRXDIR = 0x53, GS_HWREG = 0x54,
    GS_SIGNAL = 0x60, GS_FINISH = 0x61, GS_LABEL = 0x62,
};

#define PRIM_POINT 0
#define PRIM_LINE 1
#define PRIM_LINESTRIP 2
#define PRIM_TRI 3
#define PRIM_TRISTRIP 4
#define PRIM_TRIFAN 5
#define PRIM_SPRITE 6
#define PRIM_IIP (1u << 3)
#define PRIM_TME (1u << 4)
#define PRIM_ABE (1u << 6)
#define PRIM_FST (1u << 8)
#define PRIM_CTXT2 (1u << 9)

typedef struct { s32 x, y; u32 z; u32 rgba; float s, t, q; u16 u, v;
                 float fog; float fx, fy, fzn; u8 nat; } gs_vtx;

static u8 *gs_vram;
static u64 gs_reg[0x64];
static u64 gs_state_ver = 1;
static u64 fs_memo_hits, fs_memo_misses;
static u32 gs_prim;
static u32 gs_rgba;
static float gs_st_s, gs_st_t, gs_q = 1.0f;
static u16 gs_u, gs_v;
static float gs_fog = 1.0f;
static gs_vtx gs_queue[3];
static int gs_qn, gs_strip_parity;
static u64 gs_stat_prims, gs_stat_pixels, gs_stat_regs;
u64 ps2_gs_vtx_draw, ps2_gs_vtx_adc;
static u64 gs_stat_trx, gs_stat_trxpix;
static u64 gs_stat_deint;
static int no_deinterlace;
static u64 gs_reg_hist[0x64];

static u32 trx_x, trx_y, trx_left;

#define GS_PAGE_BYTES 8192u
#define GS_VRAM_PAGES (GS_VRAM_SIZE / GS_PAGE_BYTES)
static u32 gs_page_epoch[GS_VRAM_PAGES];
static u32 gs_epoch_seq;
static u64 gs_validation_seq = 1;

static u32 gs_page_owner[GS_VRAM_PAGES];
static u16 gs_page_owner_bw[GS_VRAM_PAGES];
static u8  gs_page_owner_psm[GS_VRAM_PAGES];
static u32 gs_dirty_owner;
static u16 gs_dirty_owner_bw;
static u8  gs_dirty_owner_psm;

#define GS_SHADOW_MAX 1024
static struct {
    u32 owner;
    u16 owner_bw;
    u16 next;
    u32 page;
    u32 epoch;
    u8 *data;
} gs_shadow[GS_SHADOW_MAX];
static u32 gs_shadow_n;
static u16 gs_shadow_head[GS_VRAM_PAGES];
static u64 gs_shadow_saves, gs_shadow_serves, gs_shadow_refresh;
static u64 gs_shadow_differ, gs_shadow_same, gs_shadow_runs;
static int gs_shadow_full;

static void gs_shadow_save(u32 p) {
    u32 old = gs_page_owner[p];
    u16 obw = gs_page_owner_bw[p];
    u32 i;
    if (!old || old == gs_dirty_owner) return;
    for (i = gs_shadow_head[p]; i; i = gs_shadow[i - 1u].next)
        if (gs_shadow[i - 1u].owner == old) {
            memcpy(gs_shadow[i - 1u].data, gs_vram + (size_t)p * GS_PAGE_BYTES,
                   GS_PAGE_BYTES);
            gs_shadow[i - 1u].epoch = gs_page_epoch[p];
            gs_shadow_refresh++;
            return;
        }
    if (gs_shadow_n >= GS_SHADOW_MAX) {
        if (!gs_shadow_full) {
            gs_shadow_full = 1;
            ps2_log("gs: page shadows exhausted at %u -- buffers losing pages "
                    "from here on will read whatever overwrote them",
                    (unsigned)GS_SHADOW_MAX);
        }
        return;
    }
    {
        u8 *d = (u8 *)malloc(GS_PAGE_BYTES);
        u32 k;
        if (!d) return;
        memcpy(d, gs_vram + (size_t)p * GS_PAGE_BYTES, GS_PAGE_BYTES);
        k = gs_shadow_n++;
        gs_shadow[k].owner = old;
        gs_shadow[k].owner_bw = obw;
        gs_shadow[k].page = p;
        gs_shadow[k].epoch = gs_page_epoch[p];
        gs_shadow[k].data = d;
        gs_shadow[k].next = gs_shadow_head[p];
        gs_shadow_head[p] = (u16)(k + 1u);
        gs_shadow_saves++;
    }
}

static void gs_mark_dirty(u32 lo, u32 hi) {
    u32 p, plo, phi;
    if (hi > (u32)GS_VRAM_SIZE) hi = (u32)GS_VRAM_SIZE;
    if (lo >= hi) return;
    plo = lo / GS_PAGE_BYTES;
    phi = (hi - 1u) / GS_PAGE_BYTES;
    ++gs_epoch_seq;
    ++gs_validation_seq;
    for (p = plo; p <= phi && p < GS_VRAM_PAGES; p++) {
        gs_shadow_save(p);
        gs_page_epoch[p] = gs_epoch_seq;
        gs_page_owner[p] = gs_dirty_owner;
        gs_page_owner_bw[p] = gs_dirty_owner_bw;
        gs_page_owner_psm[p] = gs_dirty_owner_psm;
    }
}

static int gs_owner_is_self(u32 owner, u16 obw, u32 tbp, u32 tbw, u32 thi) {
    u32 ow = owner & ~1u;
    return owner != 0u && ow >= tbp && ow < thi && obw == (u16)tbw;
}

static u32 gs_range_epoch(u32 lo, u32 hi) {
    u32 p, plo, phi, m = 0;
    if (hi > (u32)GS_VRAM_SIZE) hi = (u32)GS_VRAM_SIZE;
    if (lo >= hi) return 0;
    plo = lo / GS_PAGE_BYTES;
    phi = (hi - 1u) / GS_PAGE_BYTES;
    for (p = plo; p <= phi && p < GS_VRAM_PAGES; p++)
        if (gs_page_epoch[p] > m) m = gs_page_epoch[p];
    return m;
}

static int gs_shadow_off(void);

static u32 gs_content_epoch(u32 tbp, u32 tbw, u32 lo, u32 hi) {
    u32 p, plo, phi, m = 0, own = 0;
    if (gs_shadow_off()) return gs_range_epoch(lo, hi);
    if (hi > (u32)GS_VRAM_SIZE) hi = (u32)GS_VRAM_SIZE;
    if (lo >= hi) return 0;
    plo = lo / GS_PAGE_BYTES;
    phi = (hi - 1u) / GS_PAGE_BYTES;
    if (phi >= GS_VRAM_PAGES) phi = GS_VRAM_PAGES - 1u;
    for (p = plo; p <= phi; p++)
        if (gs_owner_is_self(gs_page_owner[p], gs_page_owner_bw[p],
                             tbp, tbw, hi)) { own = 1; break; }
    if (!own) return gs_range_epoch(lo, hi);
    for (p = plo; p <= phi; p++) {
        u32 e = gs_page_epoch[p];
        if (!gs_owner_is_self(gs_page_owner[p], gs_page_owner_bw[p],
                              tbp, tbw, hi)) {
            u32 i;
            for (i = gs_shadow_head[p]; i; i = gs_shadow[i - 1u].next)
                if (gs_owner_is_self(gs_shadow[i - 1u].owner,
                                     gs_shadow[i - 1u].owner_bw,
                                     tbp, tbw, hi)) {
                    e = gs_shadow[i - 1u].epoch;
                    break;
                }
        }
        if (e > m) m = e;
    }
    return m;
}

static const u8 *gs_pagebase[GS_VRAM_PAGES];

#define GS_SHCENS_N 192
static struct { u32 tbp, pages, own, foreign, redir; u64 n; }
    gs_shcens[GS_SHCENS_N];
static unsigned gs_shcens_n;

static int gs_shadow_off(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PS2_TEX_NOSHADOW");
        v = (e && *e && *e != '0') ? 1 : 0;
        if (v) ps2_log("gs: page shadows disabled (PS2_TEX_NOSHADOW)");
    }
    return v;
}

static int gs_shadow_build(u32 tbp, u32 tbw, u32 lo, u32 hi) {
    u32 p, plo, phi, own = 0, redir = 0;
    if (gs_shadow_off()) return 0;
    if (hi > (u32)GS_VRAM_SIZE) hi = (u32)GS_VRAM_SIZE;
    if (lo >= hi) return 0;
    plo = lo / GS_PAGE_BYTES;
    phi = (hi - 1u) / GS_PAGE_BYTES;
    if (phi >= GS_VRAM_PAGES) phi = GS_VRAM_PAGES - 1u;
    for (p = plo; p <= phi; p++)
        if (gs_owner_is_self(gs_page_owner[p], gs_page_owner_bw[p],
                             tbp, tbw, hi)) { own = 1; break; }
    if (!own) return 0;
    for (p = plo; p <= phi; p++) {
        const u8 *s = gs_vram;
        if (!gs_owner_is_self(gs_page_owner[p], gs_page_owner_bw[p],
                              tbp, tbw, hi)) {
            u32 i;
            for (i = gs_shadow_head[p]; i; i = gs_shadow[i - 1u].next)
                if (gs_owner_is_self(gs_shadow[i - 1u].owner,
                                     gs_shadow[i - 1u].owner_bw,
                                     tbp, tbw, hi)) {
                    s = gs_shadow[i - 1u].data - (size_t)p * GS_PAGE_BYTES;
                    redir++;
                    if (ps2_diag_armed) {
                        if (memcmp(gs_shadow[i - 1u].data,
                                   gs_vram + (size_t)p * GS_PAGE_BYTES,
                                   GS_PAGE_BYTES) != 0) gs_shadow_differ++;
                        else gs_shadow_same++;
                    }
                    break;
                }
        }
        gs_pagebase[p] = s;
    }
    if (redir) gs_shadow_serves++;
    {
        u32 i, own_n = 0, foreign = 0;
        for (p = plo; p <= phi; p++)
            if (gs_owner_is_self(gs_page_owner[p], gs_page_owner_bw[p],
                                 tbp, tbw, hi)) own_n++;
            else foreign++;
        for (i = 0; i < gs_shcens_n; i++) if (gs_shcens[i].tbp == tbp) break;
        if (i == gs_shcens_n && gs_shcens_n < GS_SHCENS_N) gs_shcens_n++;
        if (i < GS_SHCENS_N) {
            gs_shcens[i].tbp = tbp;
            gs_shcens[i].pages = phi - plo + 1u;
            gs_shcens[i].own = own_n;
            gs_shcens[i].foreign = foreign;
            gs_shcens[i].redir = redir;
            gs_shcens[i].n++;
        }
    }
    return redir != 0;
}

void ps2_gs_shadow_report(void) {
    u64 held = (u64)gs_shadow_n * GS_PAGE_BYTES;
    if (!gs_shadow_saves && !gs_shadow_serves) return;
    ps2_log("GS page shadows: %u pages held (%llu KB), %llu saved, %llu "
            "refreshed, %llu decodes served from one%s",
            gs_shadow_n, (unsigned long long)(held / 1024u),
            (unsigned long long)gs_shadow_saves,
            (unsigned long long)gs_shadow_refresh,
            (unsigned long long)gs_shadow_serves,
            gs_shadow_full ? "  (POOL EXHAUSTED)" : "");
    ps2_log("GS page shadows: %llu redirected pages differ from local memory, "
            "%llu are identical, %llu block runs read through one", (unsigned long long)gs_shadow_differ,
            (unsigned long long)gs_shadow_same, (unsigned long long)gs_shadow_runs);
    {
        unsigned i, j;
        for (i = 0; i < gs_shcens_n; i++) {
            unsigned best = i;
            for (j = i + 1; j < gs_shcens_n; j++)
                if (gs_shcens[j].foreign > gs_shcens[best].foreign) best = j;
            if (best != i) {
                typeof(gs_shcens[0]) t = gs_shcens[i];
                gs_shcens[i] = gs_shcens[best];
                gs_shcens[best] = t;
            }
            if (!gs_shcens[i].foreign) break;
            ps2_log("  tex tbp=%-8u %3u pages: own %3u  taken %3u  of which "
                    "%3u recovered from a shadow   (%llu decodes)",
                    gs_shcens[i].tbp, gs_shcens[i].pages, gs_shcens[i].own,
                    gs_shcens[i].foreign, gs_shcens[i].redir,
                    (unsigned long long)gs_shcens[i].n);
        }
    }
}

static u64 gs_vram_epoch;

u32 ps2_gs_frame_count;

void ps2_gs_init(void) {
    gs_vram = (u8 *)calloc(1, GS_VRAM_SIZE);
    if (!gs_vram) ps2_fatal("cannot allocate GS VRAM");
    gs_reg[GS_PRMODECONT] = 1;
    rn_claims_init();
}

#define PSM_CT32   0
#define PSM_CT24   1
#define PSM_CT16   2
#define PSM_CT16S  10
#define PSM_T8     19
#define PSM_T4     20
#define PSM_T8H    27
#define PSM_T4HL   36
#define PSM_T4HH   44
#define PSM_Z32    48
#define PSM_Z24    49
#define PSM_Z16    50
#define PSM_Z16S   51

static u32 psm_bits(u32 psm) {
    switch (psm) {
    case PSM_CT16: case PSM_CT16S: case PSM_Z16: case PSM_Z16S: return 16;
    case PSM_T8:  return 8;
    case PSM_T4:  return 4;
    default:      return 32;
    }
}

static const u8 blk_ct32[4][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 }
};
static const u8 blk_z32[4][8] = {
    { 24, 25, 28, 29,  8,  9, 12, 13 },
    { 26, 27, 30, 31, 10, 11, 14, 15 },
    { 16, 17, 20, 21,  0,  1,  4,  5 },
    { 18, 19, 22, 23,  2,  3,  6,  7 }
};
static const u8 blk_ct16[8][4] = {
    {  0,  2,  8, 10 }, {  1,  3,  9, 11 }, {  4,  6, 12, 14 }, {  5,  7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 }
};
static const u8 blk_ct16s[8][4] = {
    {  0,  2, 16, 18 }, {  1,  3, 17, 19 }, {  8, 10, 24, 26 }, {  9, 11, 25, 27 },
    {  4,  6, 20, 22 }, {  5,  7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 }
};
static const u8 blk_z16[8][4] = {
    { 24, 26, 16, 18 }, { 25, 27, 17, 19 }, { 28, 30, 20, 22 }, { 29, 31, 21, 23 },
    {  8, 10,  0,  2 }, {  9, 11,  1,  3 }, { 12, 14,  4,  6 }, { 13, 15,  5,  7 }
};
static const u8 blk_z16s[8][4] = {
    { 24, 26,  8, 10 }, { 25, 27,  9, 11 }, { 16, 18,  0,  2 }, { 17, 19,  1,  3 },
    { 28, 30, 12, 14 }, { 29, 31, 13, 15 }, { 20, 22,  4,  6 }, { 21, 23,  5,  7 }
};

#define GS_SW32 0u
#define GS_SW16 1u
#define GS_SW8  2u
#define GS_SW4  3u

static u16 gs_swz[4][512];
static int gs_swz_ready;

static u32 gs_swz_bit(u32 cls, u32 x, u32 y) {
    u32 colh = (cls == GS_SW8 || cls == GS_SW4) ? 4u : 2u;
    u32 colnum = y / colh, cy = y % colh;
    u32 w = (x & 1u) | (((x >> 1) & 3u) << 2) | ((cy & 1u) << 1);
    u32 sub = 0, bits;
    if (colh == 4u && (((cy >> 1) & 1u) ^ (colnum & 1u))) w ^= 8u;
    switch (cls) {
    case GS_SW16: bits = 16u; sub = (x >> 3) & 1u; break;
    case GS_SW8:  bits = 8u;  sub = ((cy >> 1) & 1u) | (((x >> 3) & 1u) << 1); break;
    case GS_SW4:  bits = 4u;  sub = (((x >> 3) & 3u) << 1) | ((cy >> 1) & 1u); break;
    default:      bits = 32u; sub = 0; break;
    }
    return colnum * 512u + w * 32u + sub * bits;
}

static void gs_swz_init(void) {
    static const u32 bw[4] = { 8, 16, 16, 32 };
    static const u32 bh[4] = { 8,  8, 16, 16 };
    static const u32 bits[4] = { 32, 16, 8, 4 };
    const char *e = getenv("PS2_GS_LINEAR_BLOCK");
    int linear = e && *e && *e != '0';
    u32 c, x, y;
    for (c = 0; c < 4; c++)
        for (y = 0; y < bh[c]; y++)
            for (x = 0; x < bw[c]; x++)
                gs_swz[c][y * bw[c] + x] =
                    (u16)(linear ? (y * bw[c] + x) * bits[c]
                                 : gs_swz_bit(c, x, y));
    if (linear)
        ps2_log("gs: PS2_GS_LINEAR_BLOCK -- blocks filled linearly, "
                "the pre-swizzle model");
    gs_swz_ready = 1;
}

typedef struct {
    u32 pw, ph;
    u32 bw, bh;
    u32 bits;
    const u8 *tab;
    u32 tabcols;
    u32 pwsh, phsh, bwsh, bhsh;
    u32 pwmask, phmask, bwmask, bhmask;
    const u16 *off;
} gs_geom;

static u32 gs_log2(u32 v) {
    u32 n = 0;
    while ((1u << n) < v && n < 31u) n++;
    return n;
}

static void gs_psm_geom_build(u32 psm, gs_geom *g);
static gs_geom gs_geom_tab[64];
static int gs_geom_tab_ready;

static void gs_psm_geom(u32 psm, gs_geom *g) {
    if (PS2_UNLIKELY(!gs_geom_tab_ready)) {
        for (u32 i = 0; i < 64u; i++) gs_psm_geom_build(i, &gs_geom_tab[i]);
        gs_geom_tab_ready = 1;
    }
    *g = gs_geom_tab[psm & 63u];
}

static void gs_psm_geom_build(u32 psm, gs_geom *g) {
    g->bits = psm_bits(psm);
    switch (psm) {
    case PSM_T4:
        g->pw = 128; g->ph = 128; g->bw = 32; g->bh = 16;
        g->tab = &blk_ct16[0][0]; g->tabcols = 4;
        break;
    case PSM_T8:
        g->pw = 128; g->ph = 64; g->bw = 16; g->bh = 16;
        g->tab = &blk_ct32[0][0]; g->tabcols = 8;
        break;
    case PSM_CT16:
        g->pw = 64; g->ph = 64; g->bw = 16; g->bh = 8;
        g->tab = &blk_ct16[0][0]; g->tabcols = 4;
        break;
    case PSM_CT16S:
        g->pw = 64; g->ph = 64; g->bw = 16; g->bh = 8;
        g->tab = &blk_ct16s[0][0]; g->tabcols = 4;
        break;
    case PSM_Z16:
        g->pw = 64; g->ph = 64; g->bw = 16; g->bh = 8;
        g->tab = &blk_z16[0][0]; g->tabcols = 4;
        break;
    case PSM_Z16S:
        g->pw = 64; g->ph = 64; g->bw = 16; g->bh = 8;
        g->tab = &blk_z16s[0][0]; g->tabcols = 4;
        break;
    case PSM_Z32: case PSM_Z24:
        g->pw = 64; g->ph = 32; g->bw = 8; g->bh = 8;
        g->tab = &blk_z32[0][0]; g->tabcols = 8;
        break;
    default:
        g->pw = 64; g->ph = 32; g->bw = 8; g->bh = 8;
        g->tab = &blk_ct32[0][0]; g->tabcols = 8;
        break;
    }
    if (!gs_swz_ready) gs_swz_init();
    g->off = gs_swz[g->bits == 4u  ? GS_SW4
                  : g->bits == 8u  ? GS_SW8
                  : g->bits == 16u ? GS_SW16 : GS_SW32];

    g->pwsh = gs_log2(g->pw); g->pwmask = g->pw - 1u;
    g->phsh = gs_log2(g->ph); g->phmask = g->ph - 1u;
    g->bwsh = gs_log2(g->bw); g->bwmask = g->bw - 1u;
    g->bhsh = gs_log2(g->bh); g->bhmask = g->bh - 1u;
}

static inline u64 vram_bit_g(const gs_geom *g, u32 base, u32 ppr, u32 x, u32 y) {
    u32 page = (y >> g->phsh) * ppr + (x >> g->pwsh);
    u32 block = g->tab[((y & g->phmask) >> g->bhsh) * g->tabcols
                       + ((x & g->pwmask) >> g->bwsh)];
    u32 inblk = g->off[((y & g->bhmask) << g->bwsh) + (x & g->bwmask)];
    return (u64)base * 8u + (u64)page * 65536u + (u64)block * 2048u
         + (u64)inblk;
}

static u32 gs_page_span(const gs_geom *g, u32 bufw, u32 x1, u32 y1) {
    u32 ppr = bufw / g->pw;
    if (!ppr) ppr = 1;
    return (y1 / g->ph) * ppr + (x1 / g->pw) + 1u;
}

static u64 vram_bit(u32 base, u32 bufw, u32 x, u32 y, u32 psm) {
    gs_geom g;
    u32 ppr;
    gs_psm_geom(psm, &g);
    ppr = bufw / g.pw;
    if (!ppr) ppr = 1;
    return vram_bit_g(&g, base, ppr, x, y);
}

static u32 vram_get(u32 base, u32 bw, u32 x, u32 y, u32 psm) {
    u64 bit = vram_bit(base, bw, x, y, psm);
    u32 off = (u32)(bit >> 3), w;
    switch (psm_bits(psm)) {
    case 4:
        if (off >= GS_VRAM_SIZE) return 0;
        return (bit & 4u) ? (gs_vram[off] >> 4) : (gs_vram[off] & 0xFu);
    case 8:
        return off < GS_VRAM_SIZE ? gs_vram[off] : 0u;
    case 16:
        if (off + 2 > GS_VRAM_SIZE) return 0;
        return (u32)gs_vram[off] | ((u32)gs_vram[off + 1] << 8);
    default:
        if (off + 4 > GS_VRAM_SIZE) return 0;
        memcpy(&w, gs_vram + off, 4);
        if (psm == PSM_T8H)  return (w >> 24) & 0xFFu;
        if (psm == PSM_T4HL) return (w >> 24) & 0x0Fu;
        if (psm == PSM_T4HH) return (w >> 28) & 0x0Fu;
        return w;
    }
}

static u32 vram_watch;
static int vram_watch_ready;
static void vram_watch_init(void) {
    const char *e = getenv("PS2_WATCH_VRAM");
    vram_watch = e ? (u32)strtoul(e, NULL, 0) : 0xFFFFFFFFu;
    vram_watch_ready = 1;
}

static void vram_store_b(u32 off, u64 bit, u32 psm, u32 bits, u32 v);
static void vram_store(u32 off, u64 bit, u32 psm, u32 v) {
    vram_store_b(off, bit, psm, psm_bits(psm), v);
}
static void vram_store_b(u32 off, u64 bit, u32 psm, u32 bits, u32 v) {
    u32 w;
    switch (bits) {
    case 4:
        if (off >= GS_VRAM_SIZE) return;
        if (bit & 4u) gs_vram[off] = (u8)((gs_vram[off] & 0x0Fu) | ((v & 0xFu) << 4));
        else          gs_vram[off] = (u8)((gs_vram[off] & 0xF0u) | (v & 0xFu));
        return;
    case 8:
        if (off < GS_VRAM_SIZE) gs_vram[off] = (u8)v;
        return;
    case 16:
        if (off + 2 > GS_VRAM_SIZE) return;
        gs_vram[off] = (u8)v;
        gs_vram[off + 1] = (u8)(v >> 8);
        return;
    default:
        if (off + 4 > GS_VRAM_SIZE) return;
        if (psm == PSM_T8H || psm == PSM_T4HL || psm == PSM_T4HH) {
            memcpy(&w, gs_vram + off, 4);
            if (psm == PSM_T8H)  w = (w & 0x00FFFFFFu) | ((v & 0xFFu) << 24);
            if (psm == PSM_T4HL) w = (w & 0xF0FFFFFFu) | ((v & 0x0Fu) << 24);
            if (psm == PSM_T4HH) w = (w & 0x0FFFFFFFu) | ((v & 0x0Fu) << 28);
            memcpy(gs_vram + off, &w, 4);
            return;
        }
        if (psm == PSM_CT24 || psm == PSM_Z24) {
            memcpy(&w, gs_vram + off, 4);
            w = (w & 0xFF000000u) | (v & 0x00FFFFFFu);
            memcpy(gs_vram + off, &w, 4);
            return;
        }
        memcpy(gs_vram + off, &v, 4);
        return;
    }
}

static void vram_put(u32 base, u32 bw, u32 x, u32 y, u32 psm, u32 v) {
    u64 bit = vram_bit(base, bw, x, y, psm);
    u32 off = (u32)(bit >> 3);
    if (!vram_watch_ready) vram_watch_init();
    if (off == vram_watch)
        ps2_log("watch: frame %llu  write %08X psm=%u via base=%u bw=%u (%u,%u)",
                (unsigned long long)ps2_gs_frame_count, v, psm, base, bw, x, y);
    vram_store(off, bit, psm, v);
}

static u32 ctx_of(void) { return (gs_prim & PRIM_CTXT2) ? 1u : 0u; }

static u64 frame_reg(void) { return gs_reg[ctx_of() ? GS_FRAME_2 : GS_FRAME_1]; }
static u64 zbuf_reg(void) { return gs_reg[ctx_of() ? GS_ZBUF_2 : GS_ZBUF_1]; }
static u64 test_reg(void) { return gs_reg[ctx_of() ? GS_TEST_2 : GS_TEST_1]; }
static u64 scissor_reg(void) { return gs_reg[ctx_of() ? GS_SCISSOR_2 : GS_SCISSOR_1]; }
static u64 xyoff_reg(void) { return gs_reg[ctx_of() ? GS_XYOFFSET_2 : GS_XYOFFSET_1]; }
static u64 tex0_reg(void) { return gs_reg[ctx_of() ? GS_TEX0_2 : GS_TEX0_1]; }
static u64 alpha_reg(void) { return gs_reg[ctx_of() ? GS_ALPHA_2 : GS_ALPHA_1]; }
static u64 clamp_reg(void) { return gs_reg[ctx_of() ? GS_CLAMP_2 : GS_CLAMP_1]; }
static u64 tex1_reg(void) { return gs_reg[ctx_of() ? GS_TEX1_2 : GS_TEX1_1]; }
static u64 miptbp1_reg(void) { return gs_reg[ctx_of() ? GS_MIPTBP1_2 : GS_MIPTBP1_1]; }
static u64 miptbp2_reg(void) { return gs_reg[ctx_of() ? GS_MIPTBP2_2 : GS_MIPTBP2_1]; }

static u32 fb_base(void) { return (u32)(frame_reg() & 0x1FFull) * 2048u * 4u; }
static u32 fb_width(void) { return (u32)((frame_reg() >> 16) & 0x3Full) * 64u; }
static u32 fb_psm(void) { return (u32)((frame_reg() >> 24) & 0x3Full); }
static u32 fb_mask(void) { return (u32)(frame_reg() >> 32); }
static u32 zb_base(void) { return (u32)(zbuf_reg() & 0x1FFull) * 2048u * 4u; }
static u32 zb_enabled(void) { return ((zbuf_reg() >> 32) & 1ull) == 0ull; }

u64 ps2_gs_cur_tex0(void) { return tex0_reg(); }
u64 ps2_gs_cur_frame(void) { return frame_reg(); }
u64 ps2_gs_cur_zbuf(void) { return zbuf_reg(); }
u64 ps2_gs_cur_test(void) { return test_reg(); }

static u32 gs_tex_lod(void) {
    u64 t1 = tex1_reg();
    u32 mxl  = (u32)((t1 >> 2) & 7ull);
    u32 mmin = (u32)((t1 >> 6) & 7ull);
    s32 k;
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("PS2_NO_MIP");
        off = (e && *e && *e != '0') ? 1 : 0;
        if (off) ps2_log("GS: mipmap level selection disabled (PS2_NO_MIP)");
    }
    if (off) return 0u;
    if (!mxl || mmin < 2u || !(t1 & 1ull)) return 0u;
    k = (s32)((t1 >> 32) & 0xFFFull);
    if (k & 0x800) k -= 0x1000;
    if (k < 0) return 0u;
    k = (k + 8) >> 4;
    return (u32)k > mxl ? mxl : (u32)k;
}

static void gs_mip_level(u32 lod, u32 psm, u32 *tbp, u32 *tbw,
                         u32 *w, u32 *h) {
    u32 bits = psm_bits(psm), i;
    if (!lod) return;
    if ((tex1_reg() >> 9) & 1ull) {
        for (i = 0; i < lod; i++) {
            *tbp += *w * *h * bits / 8u;
            *tbw = *tbw > 64u ? *tbw >> 1 : 64u;
            *w = *w > 1u ? *w >> 1 : 1u;
            *h = *h > 1u ? *h >> 1 : 1u;
        }
        return;
    }
    {
        u64 m = lod <= 3u ? miptbp1_reg() : miptbp2_reg();
        u32 s = ((lod - 1u) % 3u) * 20u;
        *tbp = (u32)((m >> s) & 0x3FFFull) * 256u;
        *tbw = (u32)((m >> (s + 14u)) & 0x3Full) * 64u;
        for (i = 0; i < lod; i++) {
            *w = *w > 1u ? *w >> 1 : 1u;
            *h = *h > 1u ? *h >> 1 : 1u;
        }
    }
}

static void put_pixel(u32 x, u32 y, u32 rgba) {
    u32 w = fb_width(), psm = fb_psm();
    if (!w || !gs_vram) return;
    if (psm == PSM_CT32 || psm == PSM_CT24) {
        u32 msk = fb_mask();
        u32 old = vram_get(fb_base(), w, x, y, psm);
        rgba = (old & msk) | (rgba & ~msk);
    }
    vram_put(fb_base(), w, x, y, psm, rgba);
    gs_stat_pixels++;
}

static int z_test_and_write(u32 x, u32 y, u32 z) {
    u32 w = fb_width(), old;
    u32 zfmt = (u32)((zbuf_reg() >> 24) & 0xFull);
    u32 zpsm = zfmt == 10 ? PSM_Z16S : zfmt + PSM_Z32;
    u32 ztst = (u32)((test_reg() >> 17) & 3ull);
    if (!zb_enabled() || !w || !gs_vram) return 1;
    old = vram_get(zb_base(), w, x, y, zpsm);
    switch (ztst) {
    case 0: return 0;
    case 1: break;
    case 2: if (!(z >= old)) return 0; break;
    case 3: if (!(z > old)) return 0; break;
    }
    vram_put(zb_base(), w, x, y, zpsm, z);
    return 1;
}

static u32 sample_texture(float s, float t, float q) {
    u64 t0 = tex0_reg();
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 tbw = (u32)((t0 >> 14) & 0x3Full) * 64u;
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    u32 tw = 1u << ((t0 >> 26) & 0xFull);
    u32 th = 1u << ((t0 >> 30) & 0xFull);
    u32 x, y, off, texel;
    if (!tbw) tbw = tw;
    if (gs_prim & PRIM_FST) { x = gs_u >> 4; y = gs_v >> 4; }
    else {
        float iq = q != 0.0f ? 1.0f / q : 0.0f;
        x = (u32)(s * iq * (float)tw);
        y = (u32)(t * iq * (float)th);
    }
    x %= tw ? tw : 1u;
    y %= th ? th : 1u;
    (void)off;
    texel = vram_get(tbp, tbw, x, y, psm);
    return texel;
}

static u32 blend(u32 src, u32 dst) {
    u64 a = alpha_reg();
    u32 A = (u32)(a & 3ull), B = (u32)((a >> 2) & 3ull);
    u32 C = (u32)((a >> 4) & 3ull), D = (u32)((a >> 6) & 3ull);
    u32 fix = (u32)((a >> 32) & 0xFFull);
    u32 out = 0;
    for (int i = 0; i < 3; i++) {
        s32 ca = (s32)((src >> (i * 8)) & 0xFF);
        s32 cb = (s32)((dst >> (i * 8)) & 0xFF);
        s32 va = A == 0 ? ca : (A == 1 ? cb : 0);
        s32 vb = B == 0 ? ca : (B == 1 ? cb : 0);
        s32 vd = D == 0 ? ca : (D == 1 ? cb : 0);
        s32 vc = C == 0 ? (s32)((src >> 24) & 0xFF)
                        : (C == 1 ? (s32)((dst >> 24) & 0xFF) : (s32)fix);
        s32 r = (((va - vb) * vc) >> 7) + vd;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        out |= (u32)r << (i * 8);
    }
    return out | (src & 0xFF000000u);
}

static void shade_pixel(s32 x, s32 y, u32 z, u32 rgba, float s, float t, float q) {
    u64 sc = scissor_reg();
    u32 x0 = (u32)(sc & 0x7FFull), x1 = (u32)((sc >> 16) & 0x7FFull);
    u32 y0 = (u32)((sc >> 32) & 0x7FFull), y1 = (u32)((sc >> 48) & 0x7FFull);
    u32 col = rgba;
    if (x < (s32)x0 || x > (s32)x1 || y < (s32)y0 || y > (s32)y1) return;
    if (x < 0 || y < 0) return;
    if (gs_prim & PRIM_TME) {
        u32 tex = sample_texture(s, t, q);
        u32 o = 0;
        for (int i = 0; i < 4; i++) {
            u32 ct = (tex >> (i * 8)) & 0xFF;
            u32 cf = (col >> (i * 8)) & 0xFF;
            u32 v = (ct * cf) >> 7;
            if (v > 255) v = 255;
            o |= v << (i * 8);
        }
        col = o;
    }
    if (!z_test_and_write((u32)x, (u32)y, z)) return;
    if (gs_prim & PRIM_ABE) {
        u32 dst = vram_get(fb_base(), fb_width(), (u32)x, (u32)y,
                           (u32)((frame_reg() >> 24) & 0x3F));
        col = blend(col, dst);
    }
    put_pixel((u32)x, (u32)y, col);
}

static void draw_sprite(const gs_vtx *a, const gs_vtx *b) {
    s32 x0 = a->x >> 4, y0 = a->y >> 4, x1 = b->x >> 4, y1 = b->y >> 4;
    s32 t, x, y;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 > 4096 || y1 - y0 > 4096) return;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float fs = 0.0f, ft = 0.0f;
            if (x1 > x0 && y1 > y0) {
                fs = a->s + (b->s - a->s) * (float)(x - x0) / (float)(x1 - x0);
                ft = a->t + (b->t - a->t) * (float)(y - y0) / (float)(y1 - y0);
            }
            shade_pixel(x, y, b->z, b->rgba, fs, ft, b->q);
        }
    gs_stat_prims++;
}

static void draw_triangle(const gs_vtx *v0, const gs_vtx *v1, const gs_vtx *v2) {
    s32 minx, maxx, miny, maxy, x, y;
    float ax = (float)v0->x / 16.0f, ay = (float)v0->y / 16.0f;
    float bx = (float)v1->x / 16.0f, by = (float)v1->y / 16.0f;
    float cx = (float)v2->x / 16.0f, cy = (float)v2->y / 16.0f;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) return;
    minx = (s32)((v0->x < v1->x ? (v0->x < v2->x ? v0->x : v2->x)
                                : (v1->x < v2->x ? v1->x : v2->x)) >> 4);
    maxx = (s32)((v0->x > v1->x ? (v0->x > v2->x ? v0->x : v2->x)
                                : (v1->x > v2->x ? v1->x : v2->x)) >> 4);
    miny = (s32)((v0->y < v1->y ? (v0->y < v2->y ? v0->y : v2->y)
                                : (v1->y < v2->y ? v1->y : v2->y)) >> 4);
    maxy = (s32)((v0->y > v1->y ? (v0->y > v2->y ? v0->y : v2->y)
                                : (v1->y > v2->y ? v1->y : v2->y)) >> 4);
    if (maxx - minx > 4096 || maxy - miny > 4096) return;
    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / area;
            float w1 = ((cx - bx) * (py - by) - (cy - by) * (px - bx)) / area;
            float w2 = 1.0f - w0 - w1;
            float l0, l1, l2;
            u32 col, z;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            l0 = w1; l1 = w2; l2 = w0;
            z = (u32)(l0 * (float)v0->z + l1 * (float)v1->z + l2 * (float)v2->z);
            if (gs_prim & PRIM_IIP) {
                u32 o = 0;
                for (int i = 0; i < 4; i++) {
                    float c = l0 * (float)((v0->rgba >> (i * 8)) & 0xFF)
                            + l1 * (float)((v1->rgba >> (i * 8)) & 0xFF)
                            + l2 * (float)((v2->rgba >> (i * 8)) & 0xFF);
                    u32 ci = (u32)(c < 0.0f ? 0.0f : (c > 255.0f ? 255.0f : c));
                    o |= ci << (i * 8);
                }
                col = o;
            } else {
                col = v2->rgba;
            }
            shade_pixel(x, y, z,  col,
                        l0 * v0->s + l1 * v1->s + l2 * v2->s,
                        l0 * v0->t + l1 * v1->t + l2 * v2->t,
                        l0 * v0->q + l1 * v1->q + l2 * v2->q);
        }
    }
    gs_stat_prims++;
}

static void vk_assemble(const gs_vtx *v);

static void vertex_push(gs_vtx v, int draw);

static void vertex_kick(s32 x, s32 y, u32 z, int draw) {
    u64 off = xyoff_reg();
    gs_vtx v;
    v.x = x - (s32)(off & 0xFFFFull);
    v.y = y - (s32)((off >> 32) & 0xFFFFull);
    v.z = z;
    v.nat = 0;
    vertex_push(v, draw);
}

static float z_norm(u32 z);
static float z_norm_d(double z, int z32);

void ps2_gs_native_vertex(float x, float y, double z, float fog, int kind, int adc) {
    u64 off = xyoff_reg();
    float ox = (float)(off & 0xFFFFull) / 16.0f;
    float oy = (float)((off >> 32) & 0xFFFFull) / 16.0f;
    gs_vtx v;
    if (kind == 0) gs_fog = fog / 255.0f;
    v.fx = x - ox;
    v.fy = y - oy;
    v.x = (s32)(x * 16.0f) - (s32)(off & 0xFFFFull);
    v.y = (s32)(y * 16.0f) - (s32)((off >> 32) & 0xFFFFull);
    v.z = z <= 0.0 ? 0u : (z >= 4294967295.0 ? 0xFFFFFFFFu : (u32)z);
    v.fzn = z_norm_d(z, kind == 1);
    v.nat = 1;
    vertex_push(v, !adc);
}

static void vertex_push(gs_vtx v, int draw) {
    if (draw) ps2_gs_vtx_draw++; else ps2_gs_vtx_adc++;
    v.rgba = gs_rgba;
    v.s = gs_st_s; v.t = gs_st_t; v.q = gs_q;
    v.u = gs_u; v.v = gs_v;
    v.fog = gs_fog;

    if (gs_qn < 3) gs_queue[gs_qn++] = v;
    else { gs_queue[0] = gs_queue[1]; gs_queue[1] = gs_queue[2]; gs_queue[2] = v; }
    if (!draw) return;

    if (ps2_vk_enabled()) { vk_assemble(&v); return; }

    switch (gs_prim & 7u) {
    case PRIM_POINT:
        shade_pixel(v.x >> 4, v.y >> 4, v.z, v.rgba, v.s, v.t, v.q);
        gs_qn = 0;
        break;
    case PRIM_LINE:
    case PRIM_LINESTRIP:
        if (gs_qn >= 2) {
            gs_vtx *a = &gs_queue[gs_qn - 2], *b = &gs_queue[gs_qn - 1];
            s32 dx = (b->x - a->x) >> 4, dy = (b->y - a->y) >> 4;
            s32 steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                      ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            if (steps > 0 && steps < 4096) {
                for (s32 i = 0; i <= steps; i++)
                    shade_pixel((a->x >> 4) + dx * i / steps,
                                (a->y >> 4) + dy * i / steps,
                                b->z, b->rgba, b->s, b->t, b->q);
            }
            if ((gs_prim & 7u) == PRIM_LINE) gs_qn = 0;
            else { gs_queue[0] = *b; gs_qn = 1; }
        }
        break;
    case PRIM_TRI:
        if (gs_qn == 3) { draw_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]); gs_qn = 0; }
        break;
    case PRIM_TRISTRIP:
        if (gs_qn == 3) {
            draw_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]);
            gs_queue[0] = gs_queue[1];
            gs_queue[1] = gs_queue[2];
            gs_qn = 2;
        }
        break;
    case PRIM_TRIFAN:
        if (gs_qn == 3) {
            draw_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]);
            gs_queue[1] = gs_queue[2];
            gs_qn = 2;
        }
        break;
    case PRIM_SPRITE:
        if (gs_qn == 2) { draw_sprite(&gs_queue[0], &gs_queue[1]); gs_qn = 0; }
        else if (gs_qn == 3) { draw_sprite(&gs_queue[1], &gs_queue[2]); gs_qn = 0; }
        break;
    default:
        gs_qn = 0;
        break;
    }
}

static u64 gs_trxdir_hist[4];
static u64 gs_stat_trxlocal;

#define TRXCENSUS_N 96
static struct { u32 dbp, dbw, dpsm, w, h, x, y; u64 n; u32 first[4]; int got; }
    trxc[TRXCENSUS_N];
static unsigned trxc_n;
static int trxc_cur = -1;

int ps2_diag_armed = 0;

static void trx_census(u32 dbp, u32 dbw, u32 dpsm, u32 w, u32 h, u32 x, u32 y) {
    if (!ps2_diag_armed) return;
    unsigned i;
    for (i = 0; i < trxc_n; i++)
        if (trxc[i].dbp == dbp && trxc[i].dbw == dbw && trxc[i].dpsm == dpsm
            && trxc[i].w == w && trxc[i].h == h && trxc[i].x == x
            && trxc[i].y == y) { trxc[i].n++; trxc_cur = (int)i; return; }
    if (trxc_n >= TRXCENSUS_N) { trxc_cur = -1; return; }
    i = trxc_n++;
    trxc[i].dbp = dbp; trxc[i].dbw = dbw; trxc[i].dpsm = dpsm;
    trxc[i].w = w; trxc[i].h = h; trxc[i].x = x; trxc[i].y = y;
    trxc[i].n = 1;
    trxc_cur = (int)i;
}

void ps2_gs_trx_census_report(void) {
    unsigned i;
    ps2_log("GS TRXDIR: host->local %llu, local->host %llu, local->local %llu "
            "(%llu executed), deactivated %llu",
            (unsigned long long)gs_trxdir_hist[0],
            (unsigned long long)gs_trxdir_hist[1],
            (unsigned long long)gs_trxdir_hist[2],
            (unsigned long long)gs_stat_trxlocal,
            (unsigned long long)gs_trxdir_hist[3]);
    ps2_log("GS host->local transfer census (%u distinct):", trxc_n);
    for (i = 0; i < trxc_n; i++)
        ps2_log("  dbp=%-7u dbw=%-4u psm=%-3u %4ux%-4u at (%u,%u)  x%llu  "
                "first qw %08X%08X %08X%08X",
                trxc[i].dbp, trxc[i].dbw, trxc[i].dpsm, trxc[i].w, trxc[i].h,
                trxc[i].x, trxc[i].y, (unsigned long long)trxc[i].n,
                trxc[i].first[1], trxc[i].first[0],
                trxc[i].first[3], trxc[i].first[2]);
}

#define GS_FILL_N 1024
static struct {
    u32 dbp, dbw, psm;
    u32 max_w, max_h;
    u64 xfers, pixels;
    u64 first_field, last_field;
} gs_fill[GS_FILL_N];
static unsigned gs_fill_n;
static u64 gs_fill_dropped;

static void gs_fill_note(u32 dbp, u32 dbw, u32 dpsm, u32 w, u32 h) {
    unsigned i;
    for (i = 0; i < gs_fill_n; i++)
        if (gs_fill[i].dbp == dbp && gs_fill[i].dbw == dbw
            && gs_fill[i].psm == dpsm) break;
    if (i == gs_fill_n) {
        if (gs_fill_n >= GS_FILL_N) { gs_fill_dropped++; return; }
        gs_fill_n++;
        gs_fill[i].dbp = dbp; gs_fill[i].dbw = dbw; gs_fill[i].psm = dpsm;
        gs_fill[i].max_w = 0; gs_fill[i].max_h = 0;
        gs_fill[i].xfers = 0; gs_fill[i].pixels = 0;
        gs_fill[i].first_field = ps2_gs_frame_count;
    }
    if (w > gs_fill[i].max_w) gs_fill[i].max_w = w;
    if (h > gs_fill[i].max_h) gs_fill[i].max_h = h;
    gs_fill[i].xfers++;
    gs_fill[i].pixels += (u64)w * h;
    gs_fill[i].last_field = ps2_gs_frame_count;
}

void ps2_gs_fill_report(void) {
    unsigned i, j, ord[GS_FILL_N];
    ps2_log("GS cumulative fill census (%u buffers%s) -- every host->local "
            "transfer since boot, by destination.  Biggest first; the terrain "
            "atlases load during the mission load screen, so this is the only "
            "table that can say whether they were ever filled:", gs_fill_n,
            gs_fill_dropped ? " -- TABLE FULL" : "");
    for (i = 0; i < gs_fill_n; i++) ord[i] = i;
    for (i = 1; i < gs_fill_n; i++) {
        u32 k = ord[i];
        for (j = i; j > 0 && gs_fill[ord[j - 1]].pixels < gs_fill[k].pixels; j--)
            ord[j] = ord[j - 1];
        ord[j] = k;
    }
    for (i = 0; i < gs_fill_n; i++) {
        unsigned e = ord[i];
        if (i >= 64u && !(gs_fill[e].dbw == 512u && gs_fill[e].psm == 19u))
            continue;
        ps2_log("  dbp=%-8u dbw=%-4u psm=%-3u  %llu transfers, %llu pixels, "
                "biggest %ux%u, fields %llu..%llu",
                gs_fill[e].dbp, gs_fill[e].dbw, gs_fill[e].psm,
                (unsigned long long)gs_fill[e].xfers,
                (unsigned long long)gs_fill[e].pixels,
                gs_fill[e].max_w, gs_fill[e].max_h,
                (unsigned long long)gs_fill[e].first_field,
                (unsigned long long)gs_fill[e].last_field);
    }
}

#define GS_BLOCKS (GS_VRAM_SIZE / 256u)
static u32 blk_owner[GS_BLOCKS];
static u16 blk_slot[GS_BLOCKS];
static u32 blk_marking = 0xFFFFFFFFu;
static u32 blk_marking_slot;
static void uprec_block_lost(u32 slot, u32 id);

static void gs_psm_geom(u32 psm, gs_geom *g);
static inline u64 vram_bit_g(const gs_geom *g, u32 base, u32 ppr, u32 x, u32 y);

static int blk_walk(u32 dbp, u32 dbw, u32 dpsm, u32 x, u32 y, u32 w, u32 h,
                    int (*fn)(u32 blk, void *u), void *u) {
    gs_geom g;
    u32 ppr, bx, by;
    gs_psm_geom(dpsm, &g);
    if (!dbw) dbw = g.pw;
    ppr = dbw / g.pw;
    if (!ppr) ppr = 1;
    for (by = y & ~g.bhmask; by < y + h; by += g.bh)
        for (bx = x & ~g.bwmask; bx < x + w; bx += g.bw) {
            u64 bit = vram_bit_g(&g, dbp, ppr, bx, by);
            u32 blk = (u32)(bit >> 11);
            if (blk < GS_BLOCKS && fn(blk, u)) return 1;
        }
    return 0;
}

static int blk_mark(u32 blk, void *u) {
    u32 old = blk_owner[blk];
    (void)u;
    if (old != blk_marking && old != 0u && old != 0xFFFFFFFFu)
        uprec_block_lost(blk_slot[blk], old);
    blk_owner[blk] = blk_marking;
    blk_slot[blk] = (u16)blk_marking_slot;
    return 0;
}

static void gs_dirty_rect(u32 dbp, u32 dbw, u32 dpsm, u32 x, u32 y,
                          u32 w, u32 h) {
    gs_geom g;
    u32 ppr, first, last;
    if (!w || !h) return;
    blk_walk(dbp, dbw, dpsm, x, y, w, h, blk_mark, NULL);
    gs_psm_geom(dpsm, &g);
    if (!dbw) dbw = g.pw;
    ppr = dbw / g.pw;
    if (!ppr) ppr = 1;
    first = (y / g.ph) * ppr + (x / g.pw);
    last  = gs_page_span(&g, dbw, x + w - 1u, y + h - 1u) - 1u;
    if (last < first) last = first;
    gs_fill_note(dbp, dbw, dpsm, w, h);
    gs_dirty_owner = dbp | 1u;
    gs_dirty_owner_bw = (u16)dbw;
    gs_dirty_owner_psm = (u8)dpsm;
    gs_mark_dirty(dbp + first * GS_PAGE_BYTES,
                  dbp + (last + 1u) * GS_PAGE_BYTES);
    gs_dirty_owner = 0;
}

#define TRXEXT_N 512
static struct { u32 dbp, w, h, logs; int used; } trx_ext[TRXEXT_N];

static u32 trx_ext_slot(u32 dbp) {
    u32 h = (dbp >> 8) * 2654435761u;
    return (h >> 23) & (TRXEXT_N - 1u);
}

static int trx_ext_find(u32 dbp, int make) {
    u32 i = trx_ext_slot(dbp), n;
    for (n = 0; n < TRXEXT_N; n++) {
        u32 k = (i + n) & (TRXEXT_N - 1u);
        if (trx_ext[k].used && trx_ext[k].dbp == dbp) return (int)k;
        if (!trx_ext[k].used) {
            if (!make) return -1;
            trx_ext[k].used = 1;
            trx_ext[k].dbp = dbp;
            trx_ext[k].w = trx_ext[k].h = 0;
            trx_ext[k].logs = 0;
            return (int)k;
        }
    }
    {   static u32 said;
        if (make && said++ < 2u)
            ps2_log("gs: transfer-extent table full (%u) -- dbp=%u is not "
                    "being recorded", (unsigned)TRXEXT_N, dbp);
    }
    return -1;
}

static void trx_ext_note(u32 dbp, u32 x, u32 y, u32 w, u32 h) {
    int i;
    if (!w || !h) return;
    i = trx_ext_find(dbp, 1);
    if (i < 0) return;
    if (x + w > trx_ext[i].w) trx_ext[i].w = x + w;
    if (y + h > trx_ext[i].h) trx_ext[i].h = y + h;
}

static int trx_ext_get(u32 dbp, u32 *w, u32 *h) {
    int i = trx_ext_find(dbp, 0);
    if (i < 0) return 0;
    *w = trx_ext[i].w; *h = trx_ext[i].h;
    return 1;
}

static u32 trx_sum;

#define TRXLOG_N 64
static struct { u32 dbp, w, h, sum; } trx_log[TRXLOG_N];
static u32 trx_log_n;

void ps2_gs_trx_history(void) {
    u32 have = trx_log_n < TRXLOG_N ? trx_log_n : TRXLOG_N;
    u32 first = trx_log_n - have, k;
    if (!have) return;
    ps2_log("trx: last %u host->local transfers, oldest first:", have);
    for (k = 0; k < have; k++) {
        u32 i = (first + k) % TRXLOG_N;
        ps2_log("   dbp=%-8u %ux%-4u sum=%08X", trx_log[i].dbp,
                trx_log[i].w, trx_log[i].h, trx_log[i].sum);
    }
}

static gs_geom trx_g;
static u32 trx_dbp, trx_dbw, trx_dpsm, trx_ppr, trx_rrw, trx_sax, trx_npix;
static u64 trx_bb_seen, trx_rg_seen, trx_pos_seen;
static u32 trx_pixels_per_qw(u32 psm);

static void trx_geom_sync(void) {
    u64 bb = gs_reg[GS_BITBLTBUF], rg = gs_reg[GS_TRXREG];
    u64 pos = gs_reg[GS_TRXPOS];
    trx_bb_seen = bb; trx_rg_seen = rg; trx_pos_seen = pos;
    trx_dbp  = (u32)((bb >> 32) & 0x3FFFull) * 256u;
    trx_dbw  = (u32)((bb >> 48) & 0x3Full) * 64u;
    trx_dpsm = (u32)((bb >> 56) & 0x3Full);
    if (!trx_dbw) trx_dbw = 64u;
    gs_psm_geom(trx_dpsm, &trx_g);
    trx_ppr = trx_dbw / trx_g.pw;
    if (!trx_ppr) trx_ppr = 1u;
    trx_rrw = (u32)(rg & 0xFFFull);
    trx_sax = (u32)((pos >> 32) & 0x7FFull);
    trx_npix = trx_pixels_per_qw(trx_dpsm);
}

#define UPREC_N 4096u
#define UPREC_HASH 1024u
typedef struct {
    u32 dbp, dbw, dpsm, x, y, w, h;
    u32 epoch, hash, need, pos;
    u8 *bytes;
    u32 cap;
    int valid;
    u64 seq;
    u32 id;
    int lost;
    s32 next, prev;
    int chained;
    s32 lru_prev, lru_next;
    int hashed;
} uprec;
static uprec uprecs[UPREC_N];
static u32 uprec_n;
static s32 uprec_lru_head = -1, uprec_lru_tail = -1;

static void uprec_lru_unlink(u32 i) {
    uprec *r = &uprecs[i];
    if (r->lru_prev >= 0) uprecs[r->lru_prev].lru_next = r->lru_next;
    else uprec_lru_head = r->lru_next;
    if (r->lru_next >= 0) uprecs[r->lru_next].lru_prev = r->lru_prev;
    else uprec_lru_tail = r->lru_prev;
    r->lru_prev = r->lru_next = -1;
}

static void uprec_lru_push(u32 i, int newest) {
    uprec *r = &uprecs[i];
    if (newest) {
        r->lru_prev = uprec_lru_tail;
        r->lru_next = -1;
        if (uprec_lru_tail >= 0) uprecs[uprec_lru_tail].lru_next = (s32)i;
        else uprec_lru_head = (s32)i;
        uprec_lru_tail = (s32)i;
    } else {
        r->lru_next = uprec_lru_head;
        r->lru_prev = -1;
        if (uprec_lru_head >= 0) uprecs[uprec_lru_head].lru_prev = (s32)i;
        else uprec_lru_tail = (s32)i;
        uprec_lru_head = (s32)i;
    }
}
static int uprec_cur = -1;
static u64 uprec_seq;
static u32 uprec_id;
static s32 uprec_head[UPREC_HASH];
static int uprec_heads_ready;
static u64 uprec_evicted;

static u32 uprec_bucket(u32 dbp) { return (dbp / 256u * 2654435761u) >> 22; }

static void uprec_unlink(u32 slot) {
    uprec *r = &uprecs[slot];
    if (!r->chained) return;
    if (r->prev >= 0) uprecs[r->prev].next = r->next;
    else uprec_head[uprec_bucket(r->dbp)] = r->next;
    if (r->next >= 0) uprecs[r->next].prev = r->prev;
    r->chained = 0;
}

static void uprec_link(u32 slot) {
    uprec *r = &uprecs[slot];
    u32 b = uprec_bucket(r->dbp);
    r->prev = -1;
    r->next = uprec_head[b];
    if (r->next >= 0) uprecs[r->next].prev = (s32)slot;
    uprec_head[b] = (s32)slot;
    r->chained = 1;
}

static u32 psm_upload_bits(u32 psm) {
    switch (psm) {
    case 0: return 32; case 1: return 24; case 2: case 10: return 16;
    case 0x13: case 0x1B: return 8;
    case 0x14: case 0x24: case 0x2C: return 4;
    default: return 0;
    }
}

static void uprec_begin(u32 dbp, u32 dbw, u32 dpsm, u32 x, u32 y, u32 w, u32 h) {
    u32 bits = psm_upload_bits(dpsm), i, slot = UPREC_N;
    uprec_cur = -1;
    blk_marking = 0xFFFFFFFFu;
    if (!uprec_heads_ready) {
        for (i = 0; i < UPREC_HASH; i++) uprec_head[i] = -1;
        uprec_heads_ready = 1;
    }
    if (!bits || !w || !h || w * h > 4096u * 4096u) return;
    for (s32 k = uprec_head[uprec_bucket(dbp)]; k >= 0; k = uprecs[k].next) {
        uprec *r = &uprecs[k];
        if (r->dbp == dbp && r->dbw == dbw && r->dpsm == dpsm && r->x == x && r->y == y
            && r->w == w && r->h == h) { slot = (u32)k; break; }
    }
    if (slot == UPREC_N) {
        if (uprec_n < UPREC_N) {
            slot = uprec_n++;
            uprec_lru_push(slot, 0);
        } else {
            slot = (u32)uprec_lru_head;
            uprec_evicted++;
        }
        uprec_unlink(slot);
        uprecs[slot].dbp = dbp;
        uprec_link(slot);
    }
    {
        uprec *r = &uprecs[slot];
        u32 need = (u32)(((u64)w * h * bits + 7u) / 8u);
        if (need > r->cap) {
            u8 *nb = (u8 *)realloc(r->bytes, need);
            if (!nb) return;
            r->bytes = nb;
            r->cap = need;
        }
        r->dbp = dbp; r->dbw = dbw; r->dpsm = dpsm;
        r->x = x; r->y = y; r->w = w; r->h = h;
        r->need = need;
        r->pos = 0;
        r->valid = 0;
        r->seq = ++uprec_seq;
        uprec_lru_unlink(slot);
        uprec_lru_push(slot, 1);
        r->id = ++uprec_id;
        if (r->id == 0xFFFFFFFFu) r->id = ++uprec_id;
        r->lost = 0;
        blk_marking = r->id;
        blk_marking_slot = slot;
        uprec_cur = (int)slot;
    }
}

static void uprec_feed(u64 qw) {
    uprec *r = &uprecs[uprec_cur];
    u32 n = r->need - r->pos;
    if (n > 8u) n = 8u;
    memcpy(r->bytes + r->pos, &qw, n);
    r->pos += n;
}

static void uprec_finish(void) {
    uprec *r = &uprecs[uprec_cur];
    uprec_cur = -1;
    blk_marking = 0xFFFFFFFFu;
    if (r->pos < r->need) return;
    r->hashed = 0;
    r->epoch = gs_epoch_seq;
    r->valid = 1;
}

static u32 uprec_hash(uprec *r) {
    if (!r->hashed) {
        u32 h = 2166136261u;
        for (u32 i = 0; i < r->need; i++) h = (h ^ r->bytes[i]) * 16777619u;
        r->hash = h;
        r->hashed = 1;
    }
    return r->hash;
}

static void uprec_block_lost(u32 slot, u32 id) {
    if (slot < UPREC_N && uprecs[slot].id == id) uprecs[slot].lost = 1;
}

static int uprec_current(uprec *r) {
    return !r->lost;
}

static uprec *uprec_find(u32 dbp, u32 dbw, u32 dpsm, u32 need_w, u32 need_h) {
    uprec *best = NULL;
    if (!uprec_heads_ready) return NULL;
    for (s32 k = uprec_head[uprec_bucket(dbp)]; k >= 0; k = uprecs[k].next) {
        uprec *r = &uprecs[k];
        if (!r->valid || r->dbp != dbp || r->dpsm != dpsm || r->x || r->y) continue;
        if (dbw && r->dbw != dbw) continue;
        if (r->w < need_w || r->h < need_h) continue;
        if (!best || r->seq > best->seq) best = r;
    }
    return best;
}

static void trx_begin(void) {
    u64 pos = gs_reg[GS_TRXPOS], rg = gs_reg[GS_TRXREG];
    u64 bbc = gs_reg[GS_BITBLTBUF];
    uprec_begin((u32)((bbc >> 32) & 0x3FFFull) * 256u, (u32)((bbc >> 48) & 0x3Full) * 64u,
                (u32)((bbc >> 56) & 0x3Full), (u32)((pos >> 32) & 0x7FFull),
                (u32)((pos >> 48) & 0x7FFull), (u32)(rg & 0xFFFull),
                (u32)((rg >> 32) & 0xFFFull));
    trx_sum = 2166136261u;
    trx_geom_sync();
    gs_vram_epoch++;
    gs_dirty_rect((u32)((bbc >> 32) & 0x3FFFull) * 256u,
                  (u32)((bbc >> 48) & 0x3Full) * 64u,
                  (u32)((bbc >> 56) & 0x3Full),
                  (u32)((pos >> 32) & 0x7FFull), (u32)((pos >> 48) & 0x7FFull),
                  (u32)(rg & 0xFFFull), (u32)((rg >> 32) & 0xFFFull));
    gs_stat_trx++;
    trx_ext_note((u32)((bbc >> 32) & 0x3FFFull) * 256u,
                 (u32)((pos >> 32) & 0x7FFull), (u32)((pos >> 48) & 0x7FFull),
                 (u32)(rg & 0xFFFull), (u32)((rg >> 32) & 0xFFFull));
    trx_census((u32)((bbc >> 32) & 0x3FFFull) * 256u,
               (u32)((bbc >> 48) & 0x3Full) * 64u,
               (u32)((bbc >> 56) & 0x3Full),
               (u32)(rg & 0xFFFull), (u32)((rg >> 32) & 0xFFFull),
               (u32)((pos >> 32) & 0x7FFull), (u32)((pos >> 48) & 0x7FFull));
    {
        const char *w = PS2_ENV("PS2_WATCH_VRAM") ? getenv("PS2_WATCH_VRAM") : NULL;
        if (w) {
            u32 want = (u32)strtoul(w, NULL, 0);
            u32 dbp = (u32)((bbc >> 32) & 0x3FFFull) * 256u;
            if (dbp == want)
                ps2_log("watch: frame %llu  transfer -> %u  dbw=%u psm=%u %ux%u",
                        (unsigned long long)ps2_gs_frame_count, dbp,
                        (u32)((bbc >> 48) & 0x3Full) * 64u,
                        (u32)((bbc >> 56) & 0x3Full),
                        (u32)(rg & 0xFFFull), (u32)((rg >> 32) & 0xFFFull));
        }
    }
    if (PS2_ENV("PS2_TRACE_TRX")) {
        static int shot;
        if (!shot && (u32)((bbc >> 56) & 0x3Full) == PSM_T4) {
            shot = 1;
            ps2_dump_trace("first PSMT4 host->local transfer");
        }
    }
    if (ps2_verbose && gs_stat_trx < 8) {
        u64 bb = gs_reg[GS_BITBLTBUF];
        ps2_log("trx: dbp=%u dbw=%u dpsm=%u  %ux%u at (%u,%u)  dir=%u",
                (u32)((bb >> 32) & 0x3FFFull) * 256u,
                (u32)((bb >> 48) & 0x3Full) * 64u,
                (u32)((bb >> 56) & 0x3Full),
                (u32)(rg & 0xFFFull), (u32)((rg >> 32) & 0xFFFull),
                (u32)((pos >> 32) & 0x7FFull), (u32)((pos >> 48) & 0x7FFull),
                (u32)(gs_reg[GS_TRXDIR] & 3ull));
    }
    trx_x = (u32)((pos >> 32) & 0x7FFull);
    trx_y = (u32)((pos >> 48) & 0x7FFull);
    trx_left = (u32)(rg & 0xFFFull) * (u32)((rg >> 32) & 0xFFFull);
}

static u32 trx_pixels_per_qw(u32 psm) {
    switch (psm) {
    case PSM_CT32: case PSM_CT24: case PSM_Z32: case PSM_Z24: return 2u;
    case PSM_CT16: case PSM_CT16S: case PSM_Z16: case PSM_Z16S: return 4u;
    case PSM_T8:  case PSM_T8H:  return 8u;
    case PSM_T4:  case PSM_T4HL: case PSM_T4HH: return 16u;
    default: return 2u;
    }
}

static void trx_local(void) {
    u64 bb = gs_reg[GS_BITBLTBUF], pos = gs_reg[GS_TRXPOS], rg = gs_reg[GS_TRXREG];
    u32 sbp  = (u32)(bb & 0x3FFFull) * 256u;
    u32 sbw  = (u32)((bb >> 16) & 0x3Full) * 64u;
    u32 spsm = (u32)((bb >> 24) & 0x3Full);
    u32 dbp  = (u32)((bb >> 32) & 0x3FFFull) * 256u;
    u32 dbw  = (u32)((bb >> 48) & 0x3Full) * 64u;
    u32 dpsm = (u32)((bb >> 56) & 0x3Full);
    u32 ssax = (u32)(pos & 0x7FFull),        ssay = (u32)((pos >> 16) & 0x7FFull);
    u32 dsax = (u32)((pos >> 32) & 0x7FFull), dsay = (u32)((pos >> 48) & 0x7FFull);
    u32 dir  = (u32)((pos >> 59) & 3ull);
    u32 w = (u32)(rg & 0xFFFull), h = (u32)((rg >> 32) & 0xFFFull);
    u32 iy, ix;
    if (!sbw) sbw = 64u;
    if (!dbw) dbw = 64u;
    if (!gs_vram || !w || !h) return;
    gs_vram_epoch++;
    {   static int shown = -1;
        if (shown < 0) shown = getenv("PS2_RN_TEX_LOG_COPY") ? 0 : 1000;
        if (shown < 40) {
            shown++;
            ps2_log("rn-tex: local copy %u (w%u psm%u) %u,%u -> %u (w%u psm%u) %u,%u %ux%u",
                    sbp, sbw, spsm, ssax, ssay, dbp, dbw, dpsm, dsax, dsay, w, h);
        }
    }
    blk_marking = 0xFFFFFFFFu;
    gs_dirty_rect(dbp, dbw, dpsm, dsax, dsay, w, h);
    gs_stat_trxlocal++;
    for (iy = 0; iy < h; iy++) {
        u32 y = (dir == 1u || dir == 3u) ? h - 1u - iy : iy;
        for (ix = 0; ix < w; ix++) {
            u32 x = (dir >= 2u) ? w - 1u - ix : ix;
            vram_put(dbp, dbw, dsax + x, dsay + y, dpsm,
                     vram_get(sbp, sbw, ssax + x, ssay + y, spsm));
        }
    }
    gs_stat_trxpix += (u64)w * h;
}

static void trx_data_inner(u64 qw);
static void trx_data(u64 qw) {
    PS2_PHASE_BEGIN(PS2_PH_TRX);
    trx_data_inner(qw);
    PS2_PHASE_END(PS2_PH_TRX);
}
static void trx_data_inner(u64 qw) {
    if (uprec_cur >= 0) uprec_feed(qw);
    if (trxc_cur >= 0 && trxc[trxc_cur].got < 4) {
        trxc[trxc_cur].first[trxc[trxc_cur].got++] = (u32)qw;
        if (trxc[trxc_cur].got < 4)
            trxc[trxc_cur].first[trxc[trxc_cur].got++] = (u32)(qw >> 32);
    }
    if (gs_reg[GS_BITBLTBUF] != trx_bb_seen
        || gs_reg[GS_TRXREG] != trx_rg_seen
        || gs_reg[GS_TRXPOS] != trx_pos_seen)
        trx_geom_sync();
    u32 dbp = trx_dbp, dbw = trx_dbw, dpsm = trx_dpsm;
    u32 rrw = trx_rrw;
    u32 n = trx_npix;
    if (!dbw) dbw = 64u;
    if (!gs_vram || !rrw) return;
    u32 wrote = 0;
    {
        const u32 bits = trx_g.bits;
        const u32 xend = trx_sax + rrw;
        u32 sh, msk, i = 0;
        int watch, traced;
        if (!vram_watch_ready) vram_watch_init();
        watch = vram_watch != 0xFFFFFFFFu;
        traced = watch || PS2_ENV("PS2_TRACE_MPEG");
        switch (n) {
        case 2:  sh = 32; msk = 0xFFFFFFFFu; break;
        case 8:  sh = 8;  msk = 0xFFu; break;
        case 16: sh = 4;  msk = 0xFu; break;
        default: sh = 16; msk = 0xFFFFu; break;
        }
        while (i < n && trx_left) {
            const u32 xb = trx_x & trx_g.bwmask;
            const u16 *off_row = trx_g.off
                               + ((trx_y & trx_g.bhmask) << trx_g.bwsh);
            u64 blkbit = vram_bit_g(&trx_g, dbp, trx_ppr, trx_x, trx_y)
                       - off_row[xb];
            u32 run = trx_g.bw - xb;
            u32 k;
            if (trx_x < xend && run > xend - trx_x) run = xend - trx_x;
            if (run > n - i) run = n - i;
            if (run > trx_left) run = trx_left;
            if (!run) break;
            if (PS2_UNLIKELY(traced)) {
                for (k = 0; k < run; k++) {
                    u64 bit = blkbit + off_row[xb + k];
                    u32 px = (u32)((qw >> ((i + k) * sh)) & msk);
                    u32 off = (u32)(bit >> 3);
                    if (watch && off == vram_watch)
                        ps2_log("watch: frame %llu  write %08X psm=%u via base=%u "
                                "(%u,%u)", (unsigned long long)ps2_gs_frame_count,
                                px, dpsm, dbp, trx_x + k, trx_y);
                    vram_store_b(off, bit, dpsm, bits, px);
                    trx_sum = (trx_sum ^ px) * 16777619u;
                }
            } else if (dpsm == PSM_T8) {
                const u32 base = (u32)(blkbit >> 3);
                if (base <= GS_VRAM_SIZE - 256u) {
                    u8 *block = gs_vram + base;
                    u64 packed = qw >> (i * 8u);
                    for (k = 0; k < run; k++) {
                        block[off_row[xb + k] >> 3] = (u8)packed;
                        packed >>= 8;
                    }
                }
            } else {
                for (k = 0; k < run; k++) {
                    u64 bit = blkbit + off_row[xb + k];
                    u32 px = (u32)((qw >> ((i + k) * sh)) & msk);
                    vram_store_b((u32)(bit >> 3), bit, dpsm, bits, px);
                }
            }
            gs_stat_trxpix += run;
            trx_x += run;
            trx_left -= run;
            i += run;
            if (trx_x >= xend) { trx_x = trx_sax; trx_y++; }
            wrote = 1;
        }
    }
    if (wrote && !trx_left && PS2_ENV("PS2_TRACE_MPEG")) {
        u64 rgp = gs_reg[GS_TRXREG];
        u32 sl = trx_log_n++ % TRXLOG_N;
        trx_log[sl].dbp = dbp;
        trx_log[sl].w = (u32)(rgp & 0xFFFull);
        trx_log[sl].h = (u32)((rgp >> 32) & 0xFFFull);
        trx_log[sl].sum = trx_sum;
    }
    if (wrote && !trx_left)
        gs_dirty_rect(dbp, dbw, dpsm,
                      (u32)((gs_reg[GS_TRXPOS] >> 32) & 0x7FFull),
                      (u32)((gs_reg[GS_TRXPOS] >> 48) & 0x7FFull),
                      rrw, (u32)((gs_reg[GS_TRXREG] >> 32) & 0xFFFull));
    if (uprec_cur >= 0 && !trx_left) uprec_finish();
}

static u32 clut_entry_at(u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 psm, u32 idx);
static u32 clut_entry(u32 idx) {
    u64 t0 = tex0_reg();
    return clut_entry_at((u32)((t0 >> 37) & 0x3FFFull) * 256u,
                         (u32)((t0 >> 51) & 0xFull), (u32)((t0 >> 55) & 1ull),
                         (u32)((t0 >> 56) & 0x1Full), (u32)((t0 >> 20) & 0x3Full),
                         idx);
}

int ps2_gs_decode_indexed(u32 tbp, u32 tbw, u32 psm, u32 w, u32 h, u32 cbp,
                          u32 cpsm, u8 *rgba) {
    u32 bw = tbw ? tbw * 64u : 64u;
    if (psm != PSM_T8 && psm != PSM_T4 && psm != PSM_T8H && psm != PSM_T4HL
        && psm != PSM_T4HH)
        return -1;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u32 idx = vram_get(tbp, bw, x, y, psm);
            u32 c = clut_entry_at(cbp, cpsm, 0u, 0u, psm, idx);
            u8 *o = rgba + ((size_t)y * w + x) * 4u;
            o[0] = (u8)c; o[1] = (u8)(c >> 8); o[2] = (u8)(c >> 16);
            o[3] = (u8)(c >> 24);
        }
    return 0;
}

static u32 clut_entry_at(u32 cbp, u32 cpsm, u32 csm, u32 csa, u32 psm, u32 idx) {
    int four = (psm == PSM_T4 || psm == PSM_T4HL || psm == PSM_T4HH);
    u32 e = csa * 16u + idx;
    u32 cw, cx, cy, raw;

    if (csm) {
        u64 tc = gs_reg[GS_TEXCLUT];
        cw = (u32)(tc & 0x3Full) * 64u;
        if (!cw) cw = 64u;
        cx = (u32)(((tc >> 6) & 0x3Full) * 16ull) + e;
        cy = (u32)((tc >> 12) & 0x3FFull);
    } else if (four) {
        cw = 64u;
        cx = e % 8u;
        cy = e / 8u;
    } else {
        u32 p = (e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1);
        cw = 64u;
        cx = p % 16u;
        cy = p / 16u;
    }

    if (cpsm == PSM_CT16 || cpsm == PSM_CT16S) {
        u32 r, g, b, a;
        raw = vram_get(cbp, cw, cx, cy, PSM_CT16);
        r = (raw & 0x1Fu) << 3;
        g = ((raw >> 5) & 0x1Fu) << 3;
        b = ((raw >> 10) & 0x1Fu) << 3;
        a = (raw >> 15) & 1u ? 0x80u : 0u;
        return r | (g << 8) | (b << 16) | (a << 24);
    }
    return vram_get(cbp, cw, cx, cy, PSM_CT32);
}

static u32 texa_alpha(u32 abit, int rgb_zero) {
    u64 texa = gs_reg[GS_TEXA];
    u32 ta0 = (u32)(texa & 0xFFull);
    u32 aem = (u32)((texa >> 15) & 1ull);
    u32 ta1 = (u32)((texa >> 32) & 0xFFull);
    if (aem && rgb_zero) return 0u;
    return abit ? ta1 : ta0;
}

static u32 texel_rgba(u32 raw, u32 psm) {
    u32 r, g, b, a;
    switch (psm) {
    case 0:
        a = (raw >> 24) & 0xFFu;
        a = a >= 128u ? 255u : a * 2u;
        return (raw & 0x00FFFFFFu) | (a << 24);
    case 1:
        a = texa_alpha(0u, (raw & 0x00FFFFFFu) == 0u);
        a = a >= 128u ? 255u : a * 2u;
        return (raw & 0x00FFFFFFu) | (a << 24);
    case 2: case 10:
        r = (raw & 0x1Fu) << 3;
        g = ((raw >> 5) & 0x1Fu) << 3;
        b = ((raw >> 10) & 0x1Fu) << 3;
        a = texa_alpha((raw >> 15) & 1u, (raw & 0x7FFFu) == 0u);
        a = a >= 128u ? 255u : a * 2u;
        return r | (g << 8) | (b << 16) | (a << 24);
    case PSM_T8: case PSM_T4: case PSM_T8H:
    case PSM_T4HL: case PSM_T4HH: {
        u32 c = clut_entry(raw & 0xFFu);
        a = (c >> 24) & 0xFFu;
        a = a >= 128u ? 255u : a * 2u;
        return (c & 0x00FFFFFFu) | (a << 24);
    }
    default:
        return raw | 0xFF000000u;
    }
}

static u8 *tex_scratch;
static u32 tex_scratch_cap;

#define TEXCACHE_N 256
#define TEXIDX_N   1024
static struct {
    u64 t0, clut;
    u32 epoch;
    u64 checked_seq;
    u32 tlo, thi;
    u32 tbw;
    u32 clo, chi;
    u32 lod;
    u32 index; float w, h; int used;
    u64 skey; u32 shash;
} texcache[TEXCACHE_N];
static u32 texcache_next;
static u16 texidx[TEXIDX_N];
static u64 texmembers[TEXIDX_N][TEXCACHE_N / 64];
static u32 texcache_index_next(u32 bucket, u32 *cursor) {
    while (*cursor < TEXCACHE_N) {
        u32 word = *cursor / 64u, bit = *cursor & 63u;
        u64 members = texmembers[bucket][word] & (~(u64)0 << bit);
        if (members) {
            u32 slot = word * 64u + (u32)__builtin_ctzll(members);
            *cursor = slot + 1u;
            return slot;
        }
        *cursor = (word + 1u) * 64u;
    }
    return TEXCACHE_N;
}
static u64 texcache_hits, texcache_misses, texcache_texels, texcache_scan;

static u32 texcache_bucket(u64 t0, u64 clut, u32 lod) {
    u64 h = t0 * 0x9E3779B97F4A7C15ull;
    h ^= clut + 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    h ^= (u64)lod * 0xD6E8FEB86659FD93ull;
    return (u32)((h >> 29) & (TEXIDX_N - 1u));
}

void ps2_gs_tex_cache_stats(u64 *hits, u64 *misses, u64 *texels, u64 *scan) {
    if (hits) *hits = texcache_hits;
    if (misses) *misses = texcache_misses;
    if (texels) *texels = texcache_texels;
    if (scan) *scan = texcache_scan;
}

#define TEXCENSUS_N 96
static struct { u32 tbp, tbw, psm, tw, th, cbp, amax_min, amax_max, nz_max; u64 n; }
    texc[TEXCENSUS_N];
static unsigned texc_n;

void ps2_gs_tex_cache_report(void) {
    u64 tot = texcache_hits + texcache_misses;
    ps2_log("GS texture cache: %llu hits, %llu decodes (%.1f%% hit)%s",
            (unsigned long long)texcache_hits,
            (unsigned long long)texcache_misses,
            tot ? 100.0 * (double)texcache_hits / (double)tot : 0.0,
            texcache_misses > texcache_hits
            ? "  <-- decoding more often than reusing" : "");
    if (fs_memo_hits + fs_memo_misses)
        ps2_log("GS draw state: worked out %llu times, reused unchanged %llu times "
                "(%.1f%%)", (unsigned long long)fs_memo_misses,
                (unsigned long long)fs_memo_hits,
                100.0 * (double)fs_memo_hits / (double)(fs_memo_hits + fs_memo_misses));
}

void ps2_gs_tex_census_report(void) {
    unsigned i;
    ps2_log("GS texture-binding census (%u distinct):", texc_n);
    for (i = 0; i < texc_n; i++)
        ps2_log("  tbp=%-7u tbw=%-4u psm=%-3u %4ux%-4u cbp=%-7u x%llu  "
                "alpha_max %u..%u  non-black<=%u/%u",
                texc[i].tbp, texc[i].tbw, texc[i].psm, texc[i].tw, texc[i].th,
                texc[i].cbp, (unsigned long long)texc[i].n,
                texc[i].amax_min, texc[i].amax_max, texc[i].nz_max,
                texc[i].tw * texc[i].th);
}

#define TEXOVL_N 256
static struct {
    u32 tbp, owner, psm, tw, th, tbw;
    u32 owner_bw, owner_psm;
    u32 pages, foreign;
    u64 n;
} texovl[TEXOVL_N];
static unsigned texovl_n;
static u64 texovl_dropped;

#define TEXSELF_N 64
static struct { u32 tbp, psm, tw, th, pages, self, foreign, unwritten; u64 n; }
    texself[TEXSELF_N];
static unsigned texself_n;

static void texovl_note(u32 tbp, u32 psm, u32 tbw, u32 tw, u32 th) {
    gs_geom g;
    u32 pages, p0, k, own = tbp | 1u;
    struct { u32 owner, bw, tpsm, n; } top[8];
    unsigned ntop = 0, i;
    if (!tw || !th) return;
    gs_psm_geom(psm, &g);
    if (!tbw) tbw = g.pw;
    pages = gs_page_span(&g, tbw, tw - 1u, th - 1u);
    if (pages < 2u) return;
    p0 = tbp / GS_PAGE_BYTES;
    for (k = 0; k < pages && p0 + k < GS_VRAM_PAGES; k++) {
        u32 o = gs_page_owner[p0 + k];
        if (!o || o == own) continue;
        for (i = 0; i < ntop; i++) if (top[i].owner == o) { top[i].n++; break; }
        if (i == ntop && ntop < 8u) {
            top[ntop].owner = o;
            top[ntop].bw = gs_page_owner_bw[p0 + k];
            top[ntop].tpsm = gs_page_owner_psm[p0 + k];
            top[ntop].n = 1;
            ntop++;
        }
    }
    {
        unsigned j;
        u32 self = 0, foreign = 0, unwritten = 0;
        for (k = 0; k < pages && p0 + k < GS_VRAM_PAGES; k++) {
            u32 o = gs_page_owner[p0 + k];
            if (!gs_page_epoch[p0 + k]) unwritten++;
            else if (o == own) self++;
            else foreign++;
        }
        for (j = 0; j < texself_n; j++)
            if (texself[j].tbp == tbp && texself[j].tw == tw
                && texself[j].th == th && texself[j].psm == psm) break;
        if (j == texself_n && texself_n < TEXSELF_N) {
            texself_n++;
            texself[j].tbp = tbp; texself[j].psm = psm;
            texself[j].tw = tw; texself[j].th = th;
            texself[j].pages = pages; texself[j].n = 0;
        }
        if (j < texself_n) {
            texself[j].self = self; texself[j].foreign = foreign;
            texself[j].unwritten = unwritten; texself[j].n++;
        }
    }
    for (i = 0; i < ntop; i++) {
        unsigned j;
        for (j = 0; j < texovl_n; j++)
            if (texovl[j].tbp == tbp && texovl[j].owner == top[i].owner
                && texovl[j].tw == tw && texovl[j].th == th) break;
        if (j == texovl_n) {
            if (texovl_n >= TEXOVL_N) { texovl_dropped++; continue; }
            texovl_n++;
            texovl[j].tbp = tbp; texovl[j].owner = top[i].owner;
            texovl[j].psm = psm; texovl[j].tw = tw; texovl[j].th = th;
            texovl[j].tbw = tbw;
            texovl[j].owner_bw = top[i].bw; texovl[j].owner_psm = top[i].tpsm;
            texovl[j].pages = pages; texovl[j].foreign = 0; texovl[j].n = 0;
        }
        if (top[i].n > texovl[j].foreign) texovl[j].foreign = top[i].n;
        texovl[j].n++;
    }
}

void ps2_gs_texovl_report(void) {
    unsigned i, j, ord[TEXOVL_N];
    if (texself_n) {
        ps2_log("GS texture page provenance -- of the pages each texture "
                "covers, how many its OWN buffer last wrote, how many another "
                "buffer wrote, and how many nothing has ever written:");
        for (i = 0; i < texself_n; i++)
            ps2_log("  tex tbp=%-8u psm=%-3u %4ux%-4u  %2u pages: "
                    "own %2u  foreign %2u  never-written %2u   (%llu decodes)",
                    texself[i].tbp, texself[i].psm, texself[i].tw,
                    texself[i].th, texself[i].pages, texself[i].self,
                    texself[i].foreign, texself[i].unwritten,
                    (unsigned long long)texself[i].n);
    }
    if (!texovl_n) {
        ps2_log("GS texture/transfer overlap: none -- every texture decoded "
                "from pages its own buffer last wrote");
        return;
    }
    ps2_log("GS texture/transfer overlap (%u distinct%s): a texture whose "
            "pixels were last written by a transfer naming a DIFFERENT buffer "
            "is drawing that buffer's picture, not its own.", texovl_n,
            texovl_dropped ? " -- TABLE FULL, more were dropped" : "");
    for (i = 0; i < texovl_n; i++) ord[i] = i;
    for (i = 1; i < texovl_n; i++) {
        u32 k = ord[i];
        for (j = i; j > 0
             && texovl[ord[j - 1]].foreign < texovl[k].foreign; j--)
            ord[j] = ord[j - 1];
        ord[j] = k;
    }
    for (i = 0; i < texovl_n; i++) {
        unsigned e = ord[i];
        u32 ob = texovl[e].tbp, ow = texovl[e].owner & ~1u;
        const char *kind =
            (ow >= ob && ow < ob + texovl[e].pages * GS_PAGE_BYTES
             && texovl[e].owner_bw == texovl[e].tbw) ? "SELF-strip"
                                                        : "OTHER-buffer";
        ps2_log("  tex tbp=%-8u psm=%-3u %4ux%-4u  <- transfer dbp=%-8u "
                "dbw=%-4u psm=%-3u [%s]  covers %u of its %u pages (%u%%)  "
                "(%llu decodes)",
                texovl[e].tbp, texovl[e].psm, texovl[e].tw, texovl[e].th,
                ow, texovl[e].owner_bw, texovl[e].owner_psm, kind,
                texovl[e].foreign, texovl[e].pages,
                texovl[e].pages ? 100u * texovl[e].foreign / texovl[e].pages : 0u,
                (unsigned long long)texovl[e].n);
    }
}

static int tex_nocache(void) {
    static int nocache = -1;
    if (nocache < 0) {
        const char *e = getenv("PS2_TEX_NOCACHE");
        nocache = (e && *e && *e != '0') ? 1 : 0;
        if (nocache) ps2_log("GS: texture cache disabled (PS2_TEX_NOCACHE)");
    }
    return nocache;
}

static int texcache_find(u64 t0, u64 clut, u32 lod, u32 *idx,
                         float *out_w, float *out_h) {
    u32 bucket = texcache_bucket(t0, clut, lod);
    u32 i, cursor = 0, cand = (u32)texidx[bucket];
    for (i = 0; ; i++) {
        u32 e, s;
        if (i == 0) {
            if (!cand) continue;
            s = cand - 1u;
        } else {
            s = texcache_index_next(bucket, &cursor);
            if (s == TEXCACHE_N) break;
            if (cand && s == cand - 1u) continue;
            texcache_scan++;
        }
        if (!texcache[s].used) continue;
        if (texcache[s].t0 != t0 || texcache[s].clut != clut) continue;
        if (texcache[s].lod != lod) continue;
        if (texcache[s].checked_seq != gs_validation_seq) {
            e = gs_content_epoch(texcache[s].tlo, texcache[s].tbw,
                             texcache[s].tlo, texcache[s].thi);
            { u32 c = gs_range_epoch(texcache[s].clo, texcache[s].chi);
              if (c > e) e = c; }
            if (e > texcache[s].epoch) continue;
            texcache[s].checked_seq = gs_validation_seq;
        }
        if (texcache[s].index != 0xFFFFFFFFu
            && !ps2_vk_texture_is(texcache[s].index, texcache[s].skey,
                                  texcache[s].shash)) {
            texmembers[bucket][s / 64u] &= ~((u64)1 << (s & 63u));
            texcache[s].used = 0;
            if (texidx[bucket] == (u16)(s + 1u)) texidx[bucket] = 0;
            continue;
        }
        texcache_hits++;
        texidx[bucket] = (u16)(s + 1u);
        *out_w = texcache[s].w;
        *out_h = texcache[s].h;
        *idx = texcache[s].index;
        return 1;
    }
    return 0;
}

static void tex_extent(u64 t0, u32 lod, u32 *tbp_io, u32 *tbw_io, u32 *tw_io,
                       u32 *th_io, float *out_w, float *out_h) {
    u32 tbp = *tbp_io, tbw = *tbw_io, tw = *tw_io, th = *th_io;
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    if (tw > 1024u) tw = 1024u;
    if (th > 1024u) th = 1024u;
    if (!tbw) tbw = tw;
    gs_mip_level(lod, psm, &tbp, &tbw, &tw, &th);
    *out_w = (float)tw;
    *out_h = (float)th;
    if (tw > tbw) tw = tbw;
    {
        u32 ew, eh;
        if (trx_ext_get(tbp, &ew, &eh)) {
            gs_geom eg;
            u32 eppr, pages, p0, k, top = 0, cols = 0;
            gs_psm_geom(psm, &eg);
            eppr = tbw / eg.pw;
            if (!eppr) eppr = 1;
            pages = gs_page_span(&eg, tbw, tw - 1u, th - 1u);
            p0 = tbp / GS_PAGE_BYTES;
            for (k = 0; k < pages && p0 + k < GS_VRAM_PAGES; k++)
                if (gs_page_epoch[p0 + k]) {
                    top = k + 1u;
                    if ((k % eppr) + 1u > cols) cols = (k % eppr) + 1u;
                }
            if (top && ew && eh
                && gs_page_span(&eg, tbw, ew - 1u, eh - 1u) < top) {
                ew = cols * eg.pw;
                eh = ((top + eppr - 1u) / eppr) * eg.ph;
            }
            if (ew && tw > ew) tw = ew;
            if (eh && th > eh) th = eh;
        }
    }
    *tbp_io = tbp; *tbw_io = tbw; *tw_io = tw; *th_io = th;
}

static u32 current_texture_inner(float *out_w, float *out_h);
static u32 current_texture(float *out_w, float *out_h) {
    u32 r;
    PS2_PHASE_BEGIN(PS2_PH_TEX);
    r = current_texture_inner(out_w, out_h);
    PS2_PHASE_END(PS2_PH_TEX);
    return r;
}

static int texture_peek(float *out_w, float *out_h, u32 *idx, u32 *dw, u32 *dh) {
    u64 t0 = tex0_reg();
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 tbw = (u32)((t0 >> 14) & 0x3Full) * 64u;
    u32 tw = 1u << ((t0 >> 26) & 0xFull);
    u32 th = 1u << ((t0 >> 30) & 0xFull);
    u32 lod = gs_tex_lod();
    if (!gs_vram) { *idx = 0xFFFFFFFFu; return 1; }
    if (!tex_nocache()
        && texcache_find(t0, gs_reg[GS_TEXCLUT], lod, idx, out_w, out_h))
        return 1;
    tex_extent(t0, lod, &tbp, &tbw, &tw, &th, out_w, out_h);
    *dw = tw;
    *dh = th;
    return 0;
}
static u32 current_texture_inner(float *out_w, float *out_h) {
    u64 t0 = tex0_reg();
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 tbw = (u32)((t0 >> 14) & 0x3Full) * 64u;
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    u32 tw = 1u << ((t0 >> 26) & 0xFull);
    u32 th = 1u << ((t0 >> 30) & 0xFull);
    u32 lod = gs_tex_lod();
    u32 need, hash = 2166136261u, y, x;
    u64 key, clut = gs_reg[GS_TEXCLUT];
    if (!gs_vram) return 0xFFFFFFFFu;
    if (!tex_nocache()) {
        u32 idx;
        if (texcache_find(t0, clut, lod, &idx, out_w, out_h)) return idx;
    }
    texcache_misses++;
    tex_extent(t0, lod, &tbp, &tbw, &tw, &th, out_w, out_h);
    texovl_note(tbp, psm, tbw, tw, th);
    need = tw * th * 4u;
    if (need > tex_scratch_cap) {
        u8 *n = (u8 *)realloc(tex_scratch, need);
        if (!n) return 0xFFFFFFFFu;
        tex_scratch = n;
        tex_scratch_cap = need;
    }
    {
        gs_geom g;
        u32 ppr, indexed = (psm == PSM_T8 || psm == PSM_T4 || psm == PSM_T8H
                            || psm == PSM_T4HL || psm == PSM_T4HH);
        u32 pal[256];
        gs_psm_geom(psm, &g);
        ppr = tbw / g.pw;
        if (!ppr) ppr = 1;
        if (indexed) {
            u32 n = (psm == PSM_T4 || psm == PSM_T4HL || psm == PSM_T4HH)
                    ? 16u : 256u;
            u32 e;
            for (e = 0; e < n; e++) {
                u32 c = clut_entry(e);
                u32 a = (c >> 24) & 0xFFu;
                a = a >= 128u ? 255u : a * 2u;
                pal[e] = (c & 0x00FFFFFFu) | (a << 24);
            }
            for (e = 0; e < (psm == PSM_T4 || psm == PSM_T4HL
                             || psm == PSM_T4HH ? 16u : 256u); e++)
                hash = (hash ^ pal[e]) * 16777619u;
            for (; n < 256u; n++) pal[n] = pal[n & 15u];
        }
        texcache_texels += (u64)tw * th;
        const int redirect = gs_shadow_build(
            tbp, tbw, tbp,
            tbp + gs_page_span(&g, tbw, tw - 1u, th - 1u) * GS_PAGE_BYTES);
        for (y = 0; y < th; y++) {
            u8 *row = tex_scratch + (size_t)y * tw * 4u;
            const u32 page_row = (y >> g.phsh) * ppr;
            const u8 *blk_row = g.tab + (size_t)((y & g.phmask) >> g.bhsh)
                                         * g.tabcols;
            const u16 *off_row = g.off + ((y & g.bhmask) << g.bwsh);
            const u64 row_bit = (u64)tbp * 8u + (u64)page_row * 65536u;
            const u32 bits = g.bits;
            const u8 *src = gs_vram;
            x = 0;
            while (x < tw) {
                u64 blkbit = row_bit
                        + (u64)(x >> g.pwsh) * 65536u
                        + (u64)blk_row[(x & g.pwmask) >> g.bwsh] * 2048u;
                u32 xb = x & g.bwmask;
                u32 run = g.bw - xb;
                u32 k;
                if (redirect) {
                    u32 pg = (u32)(blkbit >> 16);
                    src = pg < GS_VRAM_PAGES ? gs_pagebase[pg] : gs_vram;
                    if (src != gs_vram) gs_shadow_runs++;
                }
                if (run > tw - x) run = tw - x;
                if (psm == PSM_T8 || psm == PSM_T4) {
                    const u32 base = (u32)(blkbit >> 3);
                    u32 *dst = (u32 *)(row + x * 4u);
                    const u16 *offsets = off_row + xb;
                    if (base <= GS_VRAM_SIZE - 256u) {
                        const u8 *block = src + base;
                        if (psm == PSM_T8) {
                            for (k = 0; k + 4u <= run; k += 4u) {
                                const u32 r0 = block[offsets[k] >> 3];
                                const u32 r1 = block[offsets[k + 1u] >> 3];
                                const u32 r2 = block[offsets[k + 2u] >> 3];
                                const u32 r3 = block[offsets[k + 3u] >> 3];
                                dst[k] = pal[r0];
                                dst[k + 1u] = pal[r1];
                                dst[k + 2u] = pal[r2];
                                dst[k + 3u] = pal[r3];
                                hash = (((hash << 13) | (hash >> 19))
                                        ^ (r0 | (r1 << 8) | (r2 << 16) | (r3 << 24)))
                                     * 0x9E3779B1u;
                            }
                            for (; k < run; k++) {
                                const u32 raw = block[offsets[k] >> 3];
                                dst[k] = pal[raw];
                                hash = (hash ^ raw) * 16777619u;
                            }
                        } else {
                            for (k = 0; k + 4u <= run; k += 4u) {
                                const u32 b0 = offsets[k], b1 = offsets[k + 1u];
                                const u32 b2 = offsets[k + 2u], b3 = offsets[k + 3u];
                                const u32 r0 = (block[b0 >> 3] >> (b0 & 4u)) & 15u;
                                const u32 r1 = (block[b1 >> 3] >> (b1 & 4u)) & 15u;
                                const u32 r2 = (block[b2 >> 3] >> (b2 & 4u)) & 15u;
                                const u32 r3 = (block[b3 >> 3] >> (b3 & 4u)) & 15u;
                                dst[k] = pal[r0];
                                dst[k + 1u] = pal[r1];
                                dst[k + 2u] = pal[r2];
                                dst[k + 3u] = pal[r3];
                                hash = (((hash << 13) | (hash >> 19))
                                        ^ (r0 | (r1 << 8) | (r2 << 16) | (r3 << 24)))
                                     * 0x9E3779B1u;
                            }
                            for (; k < run; k++) {
                                const u32 bit = offsets[k];
                                const u32 raw = (block[bit >> 3] >> (bit & 4u)) & 15u;
                                dst[k] = pal[raw];
                                hash = (hash ^ raw) * 16777619u;
                            }
                        }
                    } else {
                        for (k = 0; k < run; k++) {
                            dst[k] = pal[0];
                            hash *= 16777619u;
                        }
                    }
                    x += run;
                    continue;
                }
                for (k = 0; k < run; k++) {
                    u64 bit = blkbit + off_row[xb + k];
                    u32 off = (u32)(bit >> 3), raw, out;
                    u32 soff = off;
                    switch (bits) {
                    case 4:
                        raw = off < GS_VRAM_SIZE
                            ? ((bit & 4u) ? (u32)(src[soff] >> 4)
                                          : (u32)(src[soff] & 0xFu)) : 0u;
                        break;
                    case 8:
                        raw = off < GS_VRAM_SIZE ? src[soff] : 0u;
                        break;
                    case 16:
                        raw = off + 2u <= GS_VRAM_SIZE
                            ? ((u32)src[soff] | ((u32)src[soff + 1] << 8)) : 0u;
                        break;
                    default: {
                        u32 w32 = 0;
                        if (off + 4u <= GS_VRAM_SIZE) memcpy(&w32, src + soff, 4);
                        if (psm == PSM_T8H)       raw = (w32 >> 24) & 0xFFu;
                        else if (psm == PSM_T4HL) raw = (w32 >> 24) & 0x0Fu;
                        else if (psm == PSM_T4HH) raw = (w32 >> 28) & 0x0Fu;
                        else                      raw = w32;
                        break;
                    }
                    }
                    out = indexed ? pal[raw & 0xFFu] : texel_rgba(raw, psm);
                    memcpy(row + (x + k) * 4u, &out, 4);
                    hash = (hash ^ raw) * 16777619u;
                }
                x += run;
            }
        }
    }
    key = ((u64)tbp << 32) | ((u64)psm << 24) | ((u64)tw << 12) | th;
    {
        const char *w = PS2_ENV("PS2_WATCH_VRAM") ? getenv("PS2_WATCH_VRAM") : NULL;
        if (w) {
            u32 want = (u32)strtoul(w, NULL, 0);
            u32 cbp = (u32)((t0 >> 37) & 0x3FFFull) * 256u;
            if (cbp == want) {
                u32 w0;
                memcpy(&w0, gs_vram + cbp, 4);
                ps2_log("watch: frame %llu  decode tbp=%u psm=%u %ux%u "
                        "reads clut @%u = %08X",
                        (unsigned long long)ps2_gs_frame_count, tbp, psm,
                        tw, th, cbp, w0);
            }
        }
    }
    if (ps2_diag_armed) {
        unsigned i;
        u32 amax = 0, nz = 0;
        for (i = 0; i < tw * th; i++) {
            if (tex_scratch[i * 4 + 3] > amax) amax = tex_scratch[i * 4 + 3];
            if (tex_scratch[i * 4] | tex_scratch[i * 4 + 1]
                | tex_scratch[i * 4 + 2]) nz++;
        }
        for (i = 0; i < texc_n; i++)
            if (texc[i].tbp == tbp && texc[i].psm == psm
                && texc[i].tw == tw && texc[i].th == th) break;
        if (i == texc_n && texc_n < TEXCENSUS_N) {
            texc_n++;
            texc[i].tbp = tbp; texc[i].tbw = tbw; texc[i].psm = psm;
            texc[i].tw = tw; texc[i].th = th; texc[i].n = 0;
            texc[i].cbp = (u32)((t0 >> 37) & 0x3FFFull) * 256u;
            texc[i].amax_min = 255; texc[i].nz_max = 0;
            if (PS2_ENV("PS2_TEX_DETAIL")) {
                u32 amin = 255;
                unsigned k;
                for (k = 0; k < tw * th; k++)
                    if (tex_scratch[k * 4 + 3] < amin) amin = tex_scratch[k * 4 + 3];
                ps2_log("tex: tbp=%u tbw=%u psm=%u %ux%u  alpha %u..%u  "
                        "non-black %u/%u | cbp=%u cpsm=%u csa=%u csm=%u cld=%u",
                        tbp, tbw, psm, tw, th, amin, amax, nz, tw * th,
                        texc[i].cbp,
                        (u32)((t0 >> 51) & 0xFull), (u32)((t0 >> 56) & 0x1Full),
                        (u32)((t0 >> 55) & 1ull), (u32)((t0 >> 61) & 7ull));
                if (psm == PSM_T4 || psm == PSM_T8) {
                    char l[2400];
                    int ln = 0;
                    unsigned n = psm == PSM_T4 ? 16u : 256u;
                    for (k = 0; k < n; k++)
                        ln += snprintf(l + ln, sizeof(l) - (size_t)ln, " %08X",
                                       clut_entry(k));
                    ps2_log("   clut:%s", l);
                    ln = 0;
                    for (k = 0; k < 32; k++)
                        ln += snprintf(l + ln, sizeof(l) - (size_t)ln, "%X",
                                       vram_get(tbp, tbw, k, 0, psm));
                    ps2_log("   row0: %s", l);
                    ln = 0;
                    for (k = 0; k < 16 && texc[i].cbp + k * 4 + 4 <= GS_VRAM_SIZE; k++) {
                        u32 w;
                        memcpy(&w, gs_vram + texc[i].cbp + k * 4, 4);
                        ln += snprintf(l + ln, sizeof(l) - (size_t)ln, " %08X", w);
                    }
                    ps2_log("   raw @%u:%s", texc[i].cbp, l);
                }
            }
        }
        if (i < texc_n) {
            texc[i].n++;
            if (amax < texc[i].amax_min) texc[i].amax_min = amax;
            if (nz > texc[i].nz_max) texc[i].nz_max = nz;
            if (amax > texc[i].amax_max) texc[i].amax_max = amax;
        }
    }
    if (ps2_verbose) {
        static u32 reported;
        if (reported < 8) {
            u32 amin = 255, amax = 0, nz = 0;
            for (u32 i = 0; i < tw * th; i++) {
                u32 a = tex_scratch[i * 4 + 3];
                if (a < amin) amin = a;
                if (a > amax) amax = a;
                if (tex_scratch[i * 4] | tex_scratch[i * 4 + 1]
                    | tex_scratch[i * 4 + 2]) nz++;
            }
            ps2_log("tex: tbp=%u tbw=%u psm=%u %ux%u  alpha %u..%u  "
                    "non-black %u/%u  cbp=%u cpsm=%u csa=%u csm=%u cld=%u",
                    tbp, tbw, psm, tw, th, amin, amax, nz, tw * th,
                    (u32)((t0 >> 37) & 0x3FFFull) * 256u,
                    (u32)((t0 >> 51) & 0x7ull), (u32)((t0 >> 56) & 0x1Full),
                    (u32)((t0 >> 55) & 1ull), (u32)((t0 >> 61) & 7ull));
            if (psm == PSM_T4 || psm == PSM_T8) {
                char rl[300];
                int rn = 0;
                for (u32 e = 0; e < 32; e++)
                    rn += snprintf(rl + rn, sizeof(rl) - (size_t)rn, "%X",
                                   vram_get(tbp, tbw, e, 0, psm));
                ps2_log("   row0: %s", rl);
                rn = 0;
                for (u32 e = 0; e < 32; e++)
                    rn += snprintf(rl + rn, sizeof(rl) - (size_t)rn, "%X",
                                   vram_get(tbp, tbw, e, 16, psm));
                ps2_log("   row16:%s", rl);
            }
            if (psm == PSM_T4 || psm == PSM_T8) {
                char l[300];
                int ln = 0;
                for (u32 e = 0; e < 16; e++)
                    ln += snprintf(l + ln, sizeof(l) - (size_t)ln, " %08X",
                                   clut_entry(e));
                ps2_log("   clut:%s", l);
            }
            reported++;
        }
    }
    if (ps2_diag_armed && PS2_ENV("PS2_DUMP_TEX")) {
        static u64 dumped[256];
        static unsigned ndumped;
        u64 k = ((u64)tbp << 24) | ((u64)tw << 12) | th;
        unsigned i;
        static int nth = -1;
        static u32 seen[256];
        if (nth < 0) {
            const char *e = getenv("PS2_DUMP_TEX_NTH");
            nth = e ? (int)strtol(e, NULL, 0) : 1;
            if (nth < 1) nth = 1;
        }
        for (i = 0; i < ndumped; i++) if (dumped[i] == k) break;
        if (i == ndumped && ndumped < 256 && tw >= 32u) {
            dumped[ndumped] = k;
            seen[ndumped] = 0;
            i = ndumped++;
        }
        if (i < ndumped) seen[i]++;
        if (i < ndumped && seen[i] == (u32)nth) {
            char nm[128];
            FILE *fp;
            snprintf(nm, sizeof nm, "ps2_tex_%u_%ux%u.ppm", tbp, tw, th);
            fp = fopen(nm, "wb");
            if (fp) {
                u32 e;
                fprintf(fp, "P6\n%u %u\n255\n", tw, th);
                for (e = 0; e < tw * th; e++) {
                    fputc(tex_scratch[e * 4 + 0], fp);
                    fputc(tex_scratch[e * 4 + 1], fp);
                    fputc(tex_scratch[e * 4 + 2], fp);
                }
                fclose(fp);
                ps2_log("tex dump: %s (psm=%u tbw=%u cbp=%u)", nm, psm, tbw,
                        (u32)((t0 >> 37) & 0x3FFFull) * 256u);
            }
            snprintf(nm, sizeof nm, "ps2_tex_%u_%ux%u_a.pgm", tbp, tw, th);
            fp = fopen(nm, "wb");
            if (fp) {
                u32 e;
                fprintf(fp, "P5\n%u %u\n255\n", tw, th);
                for (e = 0; e < tw * th; e++) fputc(tex_scratch[e * 4 + 3], fp);
                fclose(fp);
            }
        }
    }
    {
        u32 idx = ps2_vk_texture(key, hash, tex_scratch, tw, th);
        u32 slot = texcache_next++ % TEXCACHE_N;
        gs_geom g;
        u32 cbp = (u32)((t0 >> 37) & 0x3FFFull) * 256u;
        gs_psm_geom(psm, &g);
        if (texcache[slot].used) {
            u32 old_bucket = texcache_bucket(texcache[slot].t0,
                texcache[slot].clut, texcache[slot].lod);
            texmembers[old_bucket][slot / 64u] &= ~((u64)1 << (slot & 63u));
        }
        texmembers[texcache_bucket(t0, clut, lod)][slot / 64u] |= (u64)1 << (slot & 63u);
        texcache[slot].used = 1;
        texcache[slot].t0 = t0;
        texcache[slot].clut = clut;
        texcache[slot].lod = lod;
        texcache[slot].tlo = tbp;
        texcache[slot].tbw = tbw;
        texcache[slot].thi = tbp + gs_page_span(&g, tbw, tw - 1u, th - 1u)
                                 * GS_PAGE_BYTES;
        texcache[slot].clo = cbp;
        texcache[slot].chi = cbp + GS_PAGE_BYTES;
        { u32 e = gs_content_epoch(tbp, tbw, texcache[slot].tlo,
                                   texcache[slot].thi);
          u32 c = gs_range_epoch(texcache[slot].clo, texcache[slot].chi);
          texcache[slot].epoch = c > e ? c : e; }
        texcache[slot].index = idx;
        texcache[slot].skey = key;
        texcache[slot].shash = hash;
        texcache[slot].checked_seq = gs_validation_seq;
        texcache[slot].w = *out_w;
        texcache[slot].h = *out_h;
        texidx[texcache_bucket(t0, clut, lod)] = (u16)(slot + 1u);
        return idx;
    }
}

static float z_norm(u32 z) {
    u32 psm = (u32)((zbuf_reg() >> 24) & 0xFull);
    double m = (psm == 1) ? 16777215.0 : (psm >= 2 ? 65535.0 : 4294967295.0);
    double v = (double)z / m;
    if (v > 1.0) v = 1.0;
    if (v < 0.0) v = 0.0;
    return (float)v;
}

static float z_norm_d(double z, int z32) {
    u32 psm = (u32)((zbuf_reg() >> 24) & 0xFull);
    double m = (psm == 1) ? 16777215.0 : (psm >= 2 ? 65535.0 : 4294967295.0);
    double lim = z32 ? 4294967295.0 : 16777215.0;
    double v;
    if (z > lim) z = lim;
    v = z / m;
    if (v > 1.0) v = 1.0;
    if (v < 0.0) v = 0.0;
    return (float)v;
}

#define GS_RT_SLOTS 32
static u32 gs_rt_base[GS_RT_SLOTS];
static u32 gs_rt_n;
static u32 gs_rt_epoch[GS_RT_SLOTS];
static u32 gs_rt_pages[GS_RT_SLOTS];
static u32 gs_rt_zbase[GS_RT_SLOTS];
static u32 gs_rt_zepoch[GS_RT_SLOTS];
static u8 gs_rt_zframe[GS_RT_SLOTS];
static u32 gs_rt_field[GS_RT_SLOTS];
static u32 gs_rt_field_pages[GS_RT_SLOTS];
static u64 gs_rt_tex_draws, gs_rt_ztex_draws, gs_rt_tex_missed;
static u32 gs_disp_base;
u64 gs_guest_frames;

static u32 rt_slot(u32 base) {
    u32 i, victim = 0;
    u32 oldest = 0xFFFFFFFFu;
    int found = 0;
    for (i = 0; i < gs_rt_n; i++) if (gs_rt_base[i] == base) return i;
    if (gs_rt_n < GS_RT_SLOTS) {
        gs_rt_base[gs_rt_n] = base;
        gs_rt_pages[gs_rt_n] = 0;
        gs_rt_zframe[gs_rt_n] = 0;
        return gs_rt_n++;
    }
    for (i = 0; i < gs_rt_n; i++) {
        if (gs_rt_base[i] == gs_disp_base) continue;
        if (!found || gs_rt_epoch[i] < oldest) {
            oldest = gs_rt_epoch[i]; victim = i; found = 1;
        }
    }
    if (!found) return 0;
    {   static u32 said;
        if (said++ < 4u)
            ps2_log("gs: out of render-target slots (%u) -- recycling slot %u "
                    "(fbp=%u, last drawn epoch %u) for fbp=%u",
                    (unsigned)GS_RT_SLOTS, victim, gs_rt_base[victim], oldest,
                    base);
    }
    gs_rt_base[victim] = base;
    gs_rt_epoch[victim] = 0;
    gs_rt_pages[victim] = 0;
    gs_rt_zframe[victim] = 0;
    return victim;
}

static int rt_find(u32 base) {
    u32 i;
    for (i = 0; i < gs_rt_n; i++) if (gs_rt_base[i] == base) return (int)i;
    return -1;
}

static u32 gs_range_epoch(u32 lo, u32 hi);
static int psm_is_indexed(u32 psm);
static int rt_covering(u32 addr) {
    u32 i;
    int best = -1;
    for (i = 0; i < gs_rt_n; i++) {
        u32 span = (gs_rt_pages[i] ? gs_rt_pages[i] : 1u) * GS_PAGE_BYTES;
        if (!gs_rt_epoch[i] || addr < gs_rt_base[i] || addr >= gs_rt_base[i] + span)
            continue;
        if (best < 0 || gs_rt_epoch[i] > gs_rt_epoch[best]) best = (int)i;
    }
    return best;
}
static void clut_rt_probe(u64 t0, u32 draw_rt) {
    static struct { u32 tbp, cbp, psm, tw, th, fbp, cs, which, stale; u64 n; } seen[128];
    static unsigned n_seen;
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    u32 cbp = (u32)((t0 >> 37) & 0x3FFFull) * 256u;
    u32 tw = 1u << ((t0 >> 26) & 0xFull), th = 1u << ((t0 >> 30) & 0xFull);
    u32 cs = (u32)((t0 >> 51) & 0x1Full);
    u32 which = 0, stale = 0, fbp = draw_rt < GS_RT_SLOTS ? gs_rt_base[draw_rt] : 0;
    int s;
    unsigned i;
    if (psm_is_indexed(psm)) {
        s = rt_covering(cbp);
        if (s >= 0) {
            which |= 1u;
            if (gs_range_epoch(cbp, cbp + 1u) <= gs_rt_epoch[s]) stale |= 1u;
        }
    }
    s = rt_covering(tbp);
    if (s >= 0) {
        which |= 2u;
        if (gs_range_epoch(tbp, tbp + 1u) <= gs_rt_epoch[s]) stale |= 2u;
    }
    if (!which) return;
    for (i = 0; i < n_seen; i++)
        if (seen[i].tbp == tbp && seen[i].cbp == cbp && seen[i].psm == psm
            && seen[i].tw == tw && seen[i].th == th && seen[i].fbp == fbp
            && seen[i].cs == cs && seen[i].which == which && seen[i].stale == stale) {
            seen[i].n++;
            if ((seen[i].n & (seen[i].n - 1)) == 0)
                ps2_log("clut-rt: x%llu frame %llu tbp=%u psm=%u %ux%u cbp=%u cs=%X "
                        "into fbp=%u  %s%s stale=%u",
                        (unsigned long long)seen[i].n,
                        (unsigned long long)ps2_gs_frame_count, tbp, psm, tw, th,
                        cbp, cs, fbp, which & 1u ? "[clut in rt]" : "",
                        which & 2u ? "[index in rt]" : "", stale);
            return;
        }
    if (n_seen < 128u) {
        i = n_seen++;
        seen[i].tbp = tbp; seen[i].cbp = cbp; seen[i].psm = psm; seen[i].tw = tw;
        seen[i].th = th; seen[i].fbp = fbp; seen[i].cs = cs; seen[i].which = which;
        seen[i].stale = stale; seen[i].n = 1;
    }
    ps2_log("clut-rt: NEW frame %llu tbp=%u psm=%u %ux%u cbp=%u cs=%X into fbp=%u  "
            "%s%s stale=%u",
            (unsigned long long)ps2_gs_frame_count, tbp, psm, tw, th, cbp, cs, fbp,
            which & 1u ? "[clut in rt]" : "", which & 2u ? "[index in rt]" : "", stale);
}

static int rt_find_z(u32 base) {
    u32 i;
    int best = -1;
    u32 newest = 0;
    for (i = 0; i < gs_rt_n; i++) {
        if (gs_rt_zbase[i] != base || !gs_rt_zepoch[i]) continue;
        if (best < 0 || gs_rt_zepoch[i] >= newest) {
            newest = gs_rt_zepoch[i]; best = (int)i;
        }
    }
    return best;
}

static int psm_is_z(u32 psm) {
    return psm == PSM_Z32 || psm == PSM_Z24
        || psm == PSM_Z16 || psm == PSM_Z16S;
}

static int psm_is_indexed(u32 psm) {
    return psm == PSM_T8 || psm == PSM_T4
        || psm == PSM_T8H || psm == PSM_T4HL || psm == PSM_T4HH;
}

static u32 tex_reach(u32 mode, u32 minc, u32 maxc, u32 declared) {
    u32 reach;
    switch (mode) {
    case 2:  reach = maxc + 1u; break;
    case 3:  reach = (minc | maxc) + 1u; break;
    default: return declared;
    }
    if (!reach || reach > declared) return declared;
    return reach;
}

void ps2_gs_deinterlace(int off) { no_deinterlace = off; }

void ps2_gs_priv_cap_save(u64 *priv, u64 *csr, u64 *imr);
void ps2_gs_priv_cap_load(const u64 *priv, u64 csr, u64 imr);

_Static_assert(GS_VRAM_PAGES == PS2_CAP_GS_PAGES,
               "ps2_gs_capstate's page tables no longer match GS_VRAM_PAGES");
_Static_assert(GS_RT_SLOTS == (int)PS2_CAP_RT_SLOTS,
               "ps2_gs_capstate's render-target tables no longer match "
               "GS_RT_SLOTS");

void ps2_gs_cap_save(ps2_gs_capstate *st) {
    memset(st, 0, sizeof *st);
    memcpy(st->reg, gs_reg, sizeof gs_reg);
    ps2_gs_priv_cap_save(st->priv, &st->csr, &st->imr);
    memcpy(st->rt_base,   gs_rt_base,   sizeof st->rt_base);
    memcpy(st->rt_epoch,  gs_rt_epoch,  sizeof st->rt_epoch);
    memcpy(st->rt_pages,  gs_rt_pages,  sizeof st->rt_pages);
    memcpy(st->rt_zbase,  gs_rt_zbase,  sizeof st->rt_zbase);
    memcpy(st->rt_zepoch, gs_rt_zepoch, sizeof st->rt_zepoch);
    st->rt_n = gs_rt_n;
    st->disp_base = gs_disp_base;
    st->epoch_seq = gs_epoch_seq;
    st->frame_count = ps2_gs_frame_count;
    memcpy(st->page_epoch,     gs_page_epoch,     sizeof st->page_epoch);
    memcpy(st->page_owner,     gs_page_owner,     sizeof st->page_owner);
    memcpy(st->page_owner_bw,  gs_page_owner_bw,  sizeof st->page_owner_bw);
    memcpy(st->page_owner_psm, gs_page_owner_psm, sizeof st->page_owner_psm);
}

void ps2_gs_cap_load(const ps2_gs_capstate *st) {
    u32 r;
    ps2_gs_priv_cap_load(st->priv, st->csr, st->imr);
    for (r = 0; r < 0x64; r++) {
        switch (r) {
        case GS_XYZF2: case GS_XYZ2: case GS_XYZF3: case GS_XYZ3:
        case GS_TRXDIR: case GS_HWREG:
            gs_reg[r] = st->reg[r];
            break;
        default:
            ps2_gs_write_reg(r, st->reg[r]);
            break;
        }
    }
    gs_qn = 0;
    gs_strip_parity = 0;
    memcpy(gs_rt_base,   st->rt_base,   sizeof gs_rt_base);
    memcpy(gs_rt_epoch,  st->rt_epoch,  sizeof gs_rt_epoch);
    memcpy(gs_rt_pages,  st->rt_pages,  sizeof gs_rt_pages);
    memcpy(gs_rt_zbase,  st->rt_zbase,  sizeof gs_rt_zbase);
    memcpy(gs_rt_zepoch, st->rt_zepoch, sizeof gs_rt_zepoch);
    memset(gs_rt_zframe, 0, sizeof gs_rt_zframe);
    gs_rt_n = st->rt_n < GS_RT_SLOTS ? st->rt_n : GS_RT_SLOTS;
    gs_disp_base = st->disp_base;
    gs_epoch_seq = st->epoch_seq;
    ++gs_validation_seq;
    memset(texcache, 0, sizeof texcache);
    memset(texmembers, 0, sizeof texmembers);
    memset(texidx, 0, sizeof texidx);
    ps2_gs_frame_count = st->frame_count;
    memcpy(gs_page_epoch,     st->page_epoch,     sizeof gs_page_epoch);
    memcpy(gs_page_owner,     st->page_owner,     sizeof gs_page_owner);
    memcpy(gs_page_owner_bw,  st->page_owner_bw,  sizeof gs_page_owner_bw);
    memcpy(gs_page_owner_psm, st->page_owner_psm, sizeof gs_page_owner_psm);
    gs_state_ver++;
}

u8 *ps2_gs_vram_ptr(u32 *size) {
    if (size) *size = (u32)GS_VRAM_SIZE;
    return gs_vram;
}

static const char *ps2_gs_reg_name(u32 r) {
    switch (r) {
    case GS_PRIM:        return "PRIM";
    case GS_RGBAQ:       return "RGBAQ";
    case GS_ST:          return "ST";
    case GS_UV:          return "UV";
    case GS_XYZF2:       return "XYZF2";
    case GS_XYZ2:        return "XYZ2";
    case GS_TEX0_1:      return "TEX0_1";
    case GS_TEX0_2:      return "TEX0_2";
    case GS_CLAMP_1:     return "CLAMP_1";
    case GS_CLAMP_2:     return "CLAMP_2";
    case GS_FOG:         return "FOG";
    case GS_XYZF3:       return "XYZF3";
    case GS_XYZ3:        return "XYZ3";
    case GS_TEX1_1:      return "TEX1_1";
    case GS_TEX1_2:      return "TEX1_2";
    case GS_TEX2_1:      return "TEX2_1";
    case GS_TEX2_2:      return "TEX2_2";
    case GS_XYOFFSET_1:  return "XYOFFSET_1";
    case GS_XYOFFSET_2:  return "XYOFFSET_2";
    case GS_PRMODECONT:  return "PRMODECONT";
    case GS_PRMODE:      return "PRMODE";
    case GS_TEXCLUT:     return "TEXCLUT";
    case GS_SCANMSK:     return "SCANMSK";
    case GS_MIPTBP1_1:   return "MIPTBP1_1";
    case GS_MIPTBP1_2:   return "MIPTBP1_2";
    case GS_MIPTBP2_1:   return "MIPTBP2_1";
    case GS_MIPTBP2_2:   return "MIPTBP2_2";
    case GS_TEXA:        return "TEXA";
    case GS_FOGCOL:      return "FOGCOL";
    case GS_TEXFLUSH:    return "TEXFLUSH";
    case GS_SCISSOR_1:   return "SCISSOR_1";
    case GS_SCISSOR_2:   return "SCISSOR_2";
    case GS_ALPHA_1:     return "ALPHA_1";
    case GS_ALPHA_2:     return "ALPHA_2";
    case GS_DIMX:        return "DIMX";
    case GS_DTHE:        return "DTHE";
    case GS_COLCLAMP:    return "COLCLAMP";
    case GS_TEST_1:      return "TEST_1";
    case GS_TEST_2:      return "TEST_2";
    case GS_PABE:        return "PABE";
    case GS_FBA_1:       return "FBA_1";
    case GS_FBA_2:       return "FBA_2";
    case GS_FRAME_1:     return "FRAME_1";
    case GS_FRAME_2:     return "FRAME_2";
    case GS_ZBUF_1:      return "ZBUF_1";
    case GS_ZBUF_2:      return "ZBUF_2";
    case GS_BITBLTBUF:   return "BITBLTBUF";
    case GS_TRXPOS:      return "TRXPOS";
    case GS_TRXREG:      return "TRXREG";
    case GS_TRXDIR:      return "TRXDIR";
    case GS_HWREG:       return "HWREG";
    case GS_SIGNAL:      return "SIGNAL";
    case GS_FINISH:      return "FINISH";
    case GS_LABEL:       return "LABEL";
    default:             return "?";
    }
}

void ps2_vk_statecap(const char *dir);

void ps2_gs_statecap(const char *dir) {
    char p[400];
    FILE *f;
    u32 i;

    snprintf(p, sizeof p, "%s/gs.txt", dir);
    f = fopen(p, "w");
    if (f) {
        u64 fb = dispfb_reg(), d = display_reg();
        fprintf(f,
            "# GS state at the frame after the snapshot was asked for.\n"
            "# Local memory is beside this as gs_vram.bin (%u KB, in the\n"
            "# hardware's page/block/column layout -- decode it with the\n"
            "# same (base,bw,x,y,psm) arithmetic vram_get uses).\n\n",
            (u32)(GS_VRAM_SIZE / 1024));
        fprintf(f, "frame %llu   %llu prims, %llu pixels, %llu register writes\n",
                (unsigned long long)ps2_gs_frame_count,
                (unsigned long long)gs_stat_prims,
                (unsigned long long)gs_stat_pixels,
                (unsigned long long)gs_stat_regs);
        fprintf(f, "transfers %llu (%llu pixels)\n\n",
                (unsigned long long)gs_stat_trx,
                (unsigned long long)gs_stat_trxpix);

        fprintf(f, "== display ==\n");
        fprintf(f, "  PMODE=%016llX  circuit %d\n",
                (unsigned long long)ps2_gs_priv_read(0x12000000u),
                (ps2_gs_priv_read(0x12000000u) & 2ull) ? 2 : 1);
        fprintf(f, "  DISPFB=%016llX base=%u fbw=%u psm=%u dbx=%u dby=%u\n",
                (unsigned long long)fb, (u32)(fb & 0x1FFull) * 2048u * 4u,
                (u32)((fb >> 9) & 0x3Full) * 64u, (u32)((fb >> 15) & 0x1Full),
                (u32)((fb >> 32) & 0x7FFull), (u32)((fb >> 43) & 0x7FFull));
        fprintf(f, "  DISPLAY=%016llX shown %ux%u\n",
                (unsigned long long)d, ps2_gs_display_width(),
                ps2_gs_display_height());
        fprintf(f, "  DISPFB1=%016llX DISPFB2=%016llX\n\n",
                (unsigned long long)ps2_gs_priv_read(0x12000070u),
                (unsigned long long)ps2_gs_priv_read(0x12000090u));

        fprintf(f, "== render targets (%u) ==\n", gs_rt_n);
        for (i = 0; i < gs_rt_n; i++)
            fprintf(f, "  slot %-2u base %u%s\n", i, gs_rt_base[i],
                    gs_rt_base[i] == gs_disp_base ? "   <- displayed" : "");

        fprintf(f, "\n== register file (the words a draw would use now) ==\n");
        for (i = 0; i < 0x64; i++)
            if (gs_reg[i] || gs_reg_hist[i])
                fprintf(f, "  %02X %-14s = %016llX   (%llu writes)\n", i,
                        ps2_gs_reg_name(i), (unsigned long long)gs_reg[i],
                        (unsigned long long)gs_reg_hist[i]);
        fclose(f);
    }

    if (gs_vram) {
        snprintf(p, sizeof p, "%s/gs_vram.bin", dir);
        f = fopen(p, "wb");
        if (f) { fwrite(gs_vram, 1, GS_VRAM_SIZE, f); fclose(f); }
    }

    ps2_vk_statecap(dir);

    snprintf(p, sizeof p, "%s/capture.gscap", dir);
    ps2_cap_request(p, 2);
}

void ps2_gs_state_save(ps2_state_put put, void *ud) {
    put(ud, "gs_vram", gs_vram, gs_vram ? (u64)GS_VRAM_SIZE : 0u);
    put(ud, "gs_reg", gs_reg, sizeof gs_reg);
    put(ud, "gs_rtbase", gs_rt_base, sizeof gs_rt_base);
    put(ud, "gs_rtepoch", gs_rt_epoch, sizeof gs_rt_epoch);
    put(ud, "gs_pgepoch", gs_page_epoch, sizeof gs_page_epoch);
}

static u64 gs_fb_date, gs_fb_alpha_c_ad, gs_fb_alpha_cd, gs_fb_fba;
static u64 gs_fb_colclamp_wrap, gs_fb_pabe, gs_fb_blended;
static struct { u32 key; u64 n; } gs_cc_eq[64];
static u32 gs_cc_eq_n;

void ps2_gs_rt_report(void) {
    u32 i;
    char line[256] = {0};
    int n = 0;
    for (i = 0; i < gs_rt_n; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %u", gs_rt_base[i]);
    ps2_log("GS render targets (%u):%s   %llu draws sampled one, %llu fell "
            "back to local memory", gs_rt_n, line,
            (unsigned long long)gs_rt_tex_draws,
            (unsigned long long)gs_rt_tex_missed);
    {
        char zl[256] = {0};
        int zn = 0;
        u32 k;
        for (k = 0; k < gs_rt_n; k++) {
            if (zn >= (int)sizeof zl - 20) break;
            if (gs_rt_zepoch[k])
                zn += snprintf(zl + zn, sizeof zl - (size_t)zn, " %u(slot %u)",
                               gs_rt_zbase[k], k);
        }
        zl[zn] = 0;
        ps2_log("GS depth buffers:%s   %llu draws sampled one as a texture",
                zn ? zl : " none", (unsigned long long)gs_rt_ztex_draws);
    }
    ps2_log("GS destination-alpha draw states: DATE %llu, blend C=Ad %llu, "
            "blend term Cd %llu, FBA %llu",
            (unsigned long long)gs_fb_date,
            (unsigned long long)gs_fb_alpha_c_ad,
            (unsigned long long)gs_fb_alpha_cd,
            (unsigned long long)gs_fb_fba);
    ps2_log("GS blend modifiers the renderer ignores: of %llu blended draws, "
            "%llu had COLCLAMP=0 (blend result wraps at 8 bits instead of "
            "clamping) and %llu had PABE=1 (blend only where source alpha's "
            "MSB is set).  Both are currently always-clamp, always-blend.",
            (unsigned long long)gs_fb_blended,
            (unsigned long long)gs_fb_colclamp_wrap,
            (unsigned long long)gs_fb_pabe);
    {   static const char *src[4] = { "Cs", "Cd", "0", "?" };
        static const char *cf[4]  = { "As", "Ad", "FIX", "?" };
        u32 j, k;
        for (j = 0; j < gs_cc_eq_n; j++)
            for (k = j + 1; k < gs_cc_eq_n; k++)
                if (gs_cc_eq[k].n > gs_cc_eq[j].n) {
                    typeof(gs_cc_eq[0]) t = gs_cc_eq[j];
                    gs_cc_eq[j] = gs_cc_eq[k]; gs_cc_eq[k] = t;
                }
        ps2_log("GS COLCLAMP=0 blend equations (%u distinct), (A-B)*C>>7+D -- "
                "A==B means the result is just D and wrap cannot differ from "
                "clamp:", gs_cc_eq_n);
        for (j = 0; j < gs_cc_eq_n && j < 12u; j++) {
            u32 K = gs_cc_eq[j].key;
            ps2_log("   (%s-%s)*%s>>7+%s  %12llu draws%s",
                    src[K & 3u], src[(K >> 2) & 3u], cf[(K >> 4) & 3u],
                    src[(K >> 6) & 3u], (unsigned long long)gs_cc_eq[j].n,
                    ((K & 3u) == ((K >> 2) & 3u)) ? "   [A==B: wrap == clamp]"
                                                  : "");
        }
    }
    ps2_log("GS field blits deinterlaced: %llu",
            (unsigned long long)gs_stat_deint);
    ps2_log("GS DISPFB base %u -> slot %d,  PMODE %llX  DISPFB1 %llX DISPFB2 %llX",
            gs_disp_base, rt_find(gs_disp_base),
            (unsigned long long)ps2_gs_priv_read(0x12000000u),
            (unsigned long long)ps2_gs_priv_read(0x12000070u),
            (unsigned long long)ps2_gs_priv_read(0x12000090u));
}

static void fill_state_count(const ps2_vk_state *st) {
    if (st->date)          gs_fb_date++;
    if (st->abe) {
        if (st->alpha_c == 1u) gs_fb_alpha_c_ad++;
        if (st->alpha_a == 1u || st->alpha_b == 1u || st->alpha_d == 1u)
            gs_fb_alpha_cd++;
    }
    if (st->fba)           gs_fb_fba++;
    if (st->abe) {
        gs_fb_blended++;
        if (!(gs_reg[GS_COLCLAMP] & 1ull)) {
            u32 key = st->alpha_a | (st->alpha_b << 2) | (st->alpha_c << 4)
                    | (st->alpha_d << 6);
            u32 j;
            gs_fb_colclamp_wrap++;
            for (j = 0; j < gs_cc_eq_n; j++)
                if (gs_cc_eq[j].key == key) break;
            if (j == gs_cc_eq_n && gs_cc_eq_n < 64u) {
                gs_cc_eq_n++;
                gs_cc_eq[j].key = key;
                gs_cc_eq[j].n = 0;
            }
            if (j < gs_cc_eq_n) gs_cc_eq[j].n++;
        }
        if (gs_reg[GS_PABE] & 1ull)        gs_fb_pabe++;
    }
}

#define GS_TEX_DEFERRED 0xFFFFFFFEu
static int fill_defer_tex;
static u32 fill_defer_w, fill_defer_h;
static int fill_no_memo;
static struct {
    int valid, defer;
    u64 ver, validation_seq;
    u32 prim, epoch_seq, frame, emitter, disp_base;
    u32 defer_w, defer_h;
    u8 tex_hit, rt_tex_draw, rt_tex_missed, rt_ztex_draw;
    ps2_vk_state st;
} fs_memo;

static const u8 gs_reg_is_data[0x64] = {
    [0x01] = 1, [0x02] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1, [0x0A] = 1,
    [0x0C] = 1, [0x0D] = 1, [0x50] = 1, [0x51] = 1, [0x52] = 1, [0x53] = 1,
    [0x54] = 1, [0x60] = 1, [0x61] = 1, [0x62] = 1,
};

static void fill_state_compute(ps2_vk_state *st);
static void fill_state(ps2_vk_state *st) {
    static int memo_on = -1;
    u64 h0, rtd0, rtm0, rtz0;
    if (PS2_UNLIKELY(memo_on < 0)) {
        const char *e = getenv("PS2_GS_STATE_MEMO");
        memo_on = !(e && *e == '0') && !getenv("PS2_CLUT_RT");
        if (!memo_on) ps2_log("GS: draw state worked out for every primitive "
                              "(PS2_GS_STATE_MEMO=0 or PS2_CLUT_RT)");
    }
    if (memo_on && fs_memo.valid && fs_memo.ver == gs_state_ver
        && fs_memo.prim == gs_prim && fs_memo.epoch_seq == gs_epoch_seq
        && fs_memo.validation_seq == gs_validation_seq
        && fs_memo.frame == ps2_gs_frame_count && fs_memo.emitter == rn_gs_emitter
        && fs_memo.disp_base == gs_disp_base && fs_memo.defer == fill_defer_tex) {
        *st = fs_memo.st;
        fill_state_count(st);
        texcache_hits += fs_memo.tex_hit;
        gs_rt_tex_draws += fs_memo.rt_tex_draw;
        gs_rt_tex_missed += fs_memo.rt_tex_missed;
        gs_rt_ztex_draws += fs_memo.rt_ztex_draw;
        fill_defer_w = fs_memo.defer_w;
        fill_defer_h = fs_memo.defer_h;
        fs_memo_hits++;
        return;
    }
    h0 = texcache_hits + texcache_misses;
    rtd0 = gs_rt_tex_draws; rtm0 = gs_rt_tex_missed; rtz0 = gs_rt_ztex_draws;
    fill_no_memo = 0;
    fill_state_compute(st);
    fs_memo_misses++;
    if (!memo_on) return;
    fs_memo.valid = !fill_no_memo;
    fs_memo.defer = fill_defer_tex;
    fs_memo.ver = gs_state_ver;
    fs_memo.prim = gs_prim;
    fs_memo.epoch_seq = gs_epoch_seq;
    fs_memo.validation_seq = gs_validation_seq;
    fs_memo.frame = ps2_gs_frame_count;
    fs_memo.emitter = rn_gs_emitter;
    fs_memo.disp_base = gs_disp_base;
    fs_memo.defer_w = fill_defer_w;
    fs_memo.defer_h = fill_defer_h;
    fs_memo.tex_hit = (u8)(texcache_hits + texcache_misses != h0);
    fs_memo.rt_tex_draw = (u8)(gs_rt_tex_draws - rtd0);
    fs_memo.rt_tex_missed = (u8)(gs_rt_tex_missed - rtm0);
    fs_memo.rt_ztex_draw = (u8)(gs_rt_ztex_draws - rtz0);
    fs_memo.st = *st;
}

static void fill_state_compute(ps2_vk_state *st) {
    u64 tst = test_reg(), sc = scissor_reg(), al = alpha_reg(), t0 = tex0_reg();
    u64 cl = clamp_reg();
    memset(st, 0, sizeof(*st));
    st->fb_mask = fb_mask();
    st->fb_w = fb_width() ? fb_width() : 640u;
    st->fb_h = (u32)((sc >> 48) & 0x7FFull) + 1u;
    if (st->fb_h < 16u) st->fb_h = 448u;
    st->fb_psm = fb_psm();
    st->scissor[0] = (s32)(sc & 0x7FFull);
    st->scissor[1] = (s32)((sc >> 16) & 0x7FFull);
    st->scissor[2] = (s32)((sc >> 32) & 0x7FFull);
    st->scissor[3] = (s32)((sc >> 48) & 0x7FFull);
    st->ztst = (u32)((tst >> 17) & 3ull);
    st->zwrite = zb_enabled() ? 1 : 0;
    st->abe = (gs_prim & PRIM_ABE) ? 1 : 0;
    {   static int off = -1;
        if (off < 0) {
            const char *e = getenv("PS2_NO_COLCLIP");
            off = (e && *e && *e != '0') ? 1 : 0;
            if (off) ps2_log("GS: wrapping blends clamp instead (PS2_NO_COLCLIP)");
        }
        st->colclip = !off && st->abe && !(gs_reg[GS_COLCLAMP] & 1ull);
    }
    st->alpha_a = (u32)(al & 3ull);
    st->alpha_b = (u32)((al >> 2) & 3ull);
    st->alpha_c = (u32)((al >> 4) & 3ull);
    st->alpha_d = (u32)((al >> 6) & 3ull);
    st->alpha_fix = (u32)((al >> 32) & 0xFFull);
    st->ate = (u32)(tst & 1ull) ? 1 : 0;
    st->atst = (u32)((tst >> 1) & 7ull);
    st->aref = (u32)((tst >> 4) & 0xFFull);
    st->afail = (u32)((tst >> 12) & 3ull);
    st->zte = (u32)((tst >> 16) & 1ull) ? 1 : 0;
    st->fge = (gs_prim & (1u << 5)) ? 1 : 0;
    st->fogcol = (u32)(gs_reg[GS_FOGCOL] & 0xFFFFFFull);
    st->fba = (u32)(gs_reg[ctx_of() ? GS_FBA_2 : GS_FBA_1] & 1ull) ? 1 : 0;
    {   static int off = -1;
        if (off < 0) {
            const char *e = getenv("PS2_NO_DATE");
            off = (e && *e && *e != '0') ? 1 : 0;
            if (off) ps2_log("GS: destination alpha test disabled (PS2_NO_DATE)");
        }
        st->date = !off && ((tst >> 14) & 1ull) ? 1 : 0;
    }
    st->datm = (u32)((tst >> 15) & 1ull) ? 1 : 0;
    fill_state_count(st);
    st->fst = (gs_prim & PRIM_FST) ? 1 : 0;
    st->tex_lod = gs_tex_lod();
    st->tex_point = ((tex1_reg() >> 5) & 1ull) ? 0 : 1;
    st->wms  = (u32)(cl & 3ull);
    st->wmt  = (u32)((cl >> 2) & 3ull);
    st->minu = (u32)((cl >> 4) & 0x3FFull);
    st->maxu = (u32)((cl >> 14) & 0x3FFull);
    st->minv = (u32)((cl >> 24) & 0x3FFull);
    st->maxv = (u32)((cl >> 34) & 0x3FFull);
    if (st->tex_lod) {
        st->minu >>= st->tex_lod; st->maxu >>= st->tex_lod;
        st->minv >>= st->tex_lod; st->maxv >>= st->tex_lod;
    }
    st->tfx = (u32)((t0 >> 35) & 3ull);
    st->tcc = (u32)((t0 >> 34) & 1ull) ? 1 : 0;
    st->tme = (gs_prim & PRIM_TME) ? 1 : 0;
    st->tex_index = 0xFFFFFFFFu;
    st->rt = rt_slot(fb_base());
    {   static int in_disp;
        int now_disp = gs_disp_base && fb_base() == gs_disp_base;
        if (now_disp && !in_disp) gs_guest_frames++;
        in_disp = now_disp;
    }
    u32 rt_epoch_before = st->rt < GS_RT_SLOTS ? gs_rt_epoch[st->rt] : 0u;
    if (st->rt < GS_RT_SLOTS) {
        gs_rt_epoch[st->rt] = gs_epoch_seq;
        {   gs_geom fg;
            u32 pages;
            gs_psm_geom(st->fb_psm, &fg);
            pages = gs_page_span(&fg, st->fb_w, st->fb_w - 1u, st->fb_h - 1u);
            if (pages > gs_rt_pages[st->rt]) gs_rt_pages[st->rt] = pages;
            if (gs_rt_field[st->rt] != ps2_gs_frame_count) gs_rt_field_pages[st->rt] = 0;
            if (pages > gs_rt_field_pages[st->rt]) gs_rt_field_pages[st->rt] = pages;
            gs_rt_field[st->rt] = ps2_gs_frame_count;
        }
        gs_rt_zbase[st->rt] = zb_base();
        if (st->zwrite) gs_rt_zepoch[st->rt] = gs_epoch_seq;
    }
    st->tex_rt = 0;
    if (st->tme) {
        u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
        int slot = rt_find(tbp);
        int use_rt = 0;
        if (slot >= 0 && !psm_is_indexed((u32)((t0 >> 20) & 0x3Full))) {
            gs_geom g;
            u32 tbw = (u32)((t0 >> 14) & 0x3Full) * 64u;
            u32 tw  = 1u << ((t0 >> 26) & 0xFull);
            u32 th  = 1u << ((t0 >> 30) & 0xFull);
            u32 hi, rt_hi;
            gs_psm_geom((u32)((t0 >> 20) & 0x3Full), &g);
            if (!tbw) tbw = g.pw;
            tw = tex_reach(st->wms, st->minu, st->maxu, tw);
            th = tex_reach(st->wmt, st->minv, st->maxv, th);
            hi = tbp + gs_page_span(&g, tbw, tw - 1u, th - 1u) * GS_PAGE_BYTES;
            rt_hi = gs_rt_pages[slot]
                  ? tbp + gs_rt_pages[slot] * GS_PAGE_BYTES : hi;
            if (hi > rt_hi) hi = rt_hi;
            {   u32 se = gs_rt_epoch[slot];
                if ((u32)slot == st->rt) {
                    fill_no_memo = 1;
                    se = rt_epoch_before;
                    hi = rt_hi;
                }
                use_rt = se && gs_range_epoch(tbp, hi) <= se;
                if (use_rt && (u32)slot == st->rt) st->tex_self = 1;
            }
            {   static u32 no_rt = 0xFFFFFFFFu;
                if (no_rt == 0xFFFFFFFFu) {
                    const char *e = getenv("PS2_NO_RT_TEX");
                    no_rt = (e && *e) ? (u32)strtoul(e, NULL, 0) : 0u;
                    if (no_rt) ps2_log("GS: render-target texture binding "
                                       "refused for %s (PS2_NO_RT_TEX)",
                                       no_rt == 1u ? "every address"
                                                   : "one address");
                }
                if (no_rt && (no_rt == 1u || no_rt == tbp)) use_rt = 0;
            }
            if (!use_rt) gs_rt_tex_missed++;
        }
        if (psm_is_z((u32)((t0 >> 20) & 0x3Full))) {
            int zslot = rt_find_z(tbp);
            if (zslot >= 0 && (u32)zslot != st->rt) {
                st->tex_depth = 1;
                gs_rt_ztex_draws++;
                use_rt = 1;
                slot = zslot;
            }
        }
        if (use_rt) {
            st->tex_rt = (u32)slot + 1u;
            st->tex_w = (float)(1u << ((t0 >> 26) & 0xFull));
            st->tex_h = (float)(1u << ((t0 >> 30) & 0xFull));
            gs_rt_tex_draws++;
        } else {
            {   static int probe = -1;
                if (probe < 0) probe = getenv("PS2_CLUT_RT") != NULL;
                if (probe) clut_rt_probe(t0, st->rt);
            }
            if (fill_defer_tex) {
                u32 idx;
                st->tex_index = texture_peek(&st->tex_w, &st->tex_h, &idx,
                                             &fill_defer_w, &fill_defer_h)
                              ? idx : GS_TEX_DEFERRED;
            } else {
                st->tex_index = current_texture(&st->tex_w, &st->tex_h);
            }
            if (st->tex_index == 0xFFFFFFFFu) st->tme = 0;
        }
    }
    {   static int off = -1;
        int zs;
        if (off < 0) {
            const char *e = getenv("PS2_NO_ZCOPY");
            off = (e && *e && *e != '0') ? 1 : 0;
            if (off) ps2_log("GS: Z copies into depth disabled (PS2_NO_ZCOPY)");
        }
        st->zcopy = !off && st->tme && st->tex_depth && psm_is_z(st->fb_psm);
        if (st->rt < GS_RT_SLOTS) {
            if (st->zcopy) gs_rt_zframe[st->rt] = 1;
            else if (!psm_is_z(st->fb_psm)) gs_rt_zframe[st->rt] = 0;
        }
        st->zrt = st->rt;
        zs = off ? -1 : rt_find(zb_base());
        if (zs >= 0 && (u32)zs != st->rt && gs_rt_zframe[zs]) st->zrt = (u32)zs;
    }
    st->emitter = rn_gs_emitter;
}

#define DRAWCENSUS_N 2048
static struct {
    u32 path;
    u64 first_seq, last_seq;
    u32 prim, tme, ate, atst, aref, ztst, zwrite, abe, fbpsm, fbw, fbh, tfx;
    u32 fbp, zbp, zpsm, tbp, tpsm, tcc, rt, texrt;
    u32 wms, wmt, minu, maxu, minv, maxv;
    u32 afail, zte;
    u32 ba, bb, bc, bd, bfix, abe2;
    u32 colclamp;
    s32 sc[4];
    float x0, y0, x1, y1, z0, z1;
    float cmin, cmax, amin, amax;
    float s0, t0_, s1, t1;
    float q0, q1, u0, u1, v0, v1;
    u64 tex1;
    int fst;
    u64 n;
} drawc[DRAWCENSUS_N];
static unsigned drawc_n;
static u64 drawc_dropped;
static u64 drawc_seq;
u64 ps2_gs_prim_seq;

#define BLITLOG_N 128
static struct {
    u64 frame;
    u32 dst, src;
    float y0, y1, t0, t1, x0, x1, s0, s1;
} blitlog[BLITLOG_N];
static u64 blitlog_n;

void ps2_gs_blit_log_report(void) {
    u64 have = blitlog_n < BLITLOG_N ? blitlog_n : BLITLOG_N;
    u64 first = blitlog_n - have, k;
    if (!have) {
        ps2_log("GS render-target blits: none recorded");
        return;
    }
    ps2_log("GS render-target blits: last %llu of %llu, oldest first:",
            (unsigned long long)have, (unsigned long long)blitlog_n);
    for (k = 0; k < have; k++) {
        u64 i = (first + k) % BLITLOG_N;
        ps2_log("   frame %llu  rt%u <- rt%u  dst x %.1f..%.1f y %.1f..%.1f"
                "  src s %.1f..%.1f t %.1f..%.1f",
                (unsigned long long)blitlog[i].frame, blitlog[i].dst,
                blitlog[i].src, blitlog[i].x0, blitlog[i].x1,
                blitlog[i].y0, blitlog[i].y1, blitlog[i].s0, blitlog[i].s1,
                blitlog[i].t0, blitlog[i].t1);
    }
}

#define VH_BINS 14
#define VH_SAMPLES 6
static struct {
    u64 total, inside;
    u64 bin[VH_BINS];
    float worst;
    float sx0, sx1, sy0, sy1;
    float ofx, ofy;
    unsigned ns;
    float sx[VH_SAMPLES], sy[VH_SAMPLES];
} vhist[4];

void ps2_gs_census_reset(void) {
    drawc_n = 0;
    drawc_dropped = 0;
    drawc_seq = 0;
    ps2_gs_prim_seq = 0;
    texc_n = 0;
    trxc_n = 0;
    trxc_cur = -1;
    memset(drawc, 0, sizeof drawc);
    memset(vhist, 0, sizeof vhist);
}

void ps2_gs_vertex_hist_report(void);
void ps2_gs_blit_log_report(void);

void ps2_gs_draw_census_report(void) {
    unsigned i;
    ps2_log("GS draw census (%u distinct states%s), #first..#last is "
            "submission order:", drawc_n,
            drawc_dropped ? "  -- TABLE FULL, more were dropped" : "");
    if (drawc_dropped)
        ps2_log("  %llu draws had a state this table had no room for; raise "
                "DRAWCENSUS_N before trusting this list to be complete.",
                (unsigned long long)drawc_dropped);
    for (i = 0; i < drawc_n; i++) {
        ps2_log("  #%llu..%llu PATH%u rt%u fbp=%-8u fbpsm=%-3u %ux%u | prim=%u tme=%u tfx=%u "
                "tcc=%u abe=%u ate=%u/%u/%u ztst=%u zw=%u | x%llu",
                (unsigned long long)drawc[i].first_seq,
                (unsigned long long)drawc[i].last_seq,
                drawc[i].path, drawc[i].rt, drawc[i].fbp, drawc[i].fbpsm,
                drawc[i].fbw, drawc[i].fbh,
                drawc[i].prim, drawc[i].tme, drawc[i].tfx, drawc[i].tcc,
                drawc[i].abe, drawc[i].ate, drawc[i].atst, drawc[i].aref,
                drawc[i].ztst, drawc[i].zwrite,
                (unsigned long long)drawc[i].n);
        {
            static const char *af[4] = { "KEEP", "FB_ONLY", "ZB_ONLY",
                                         "RGB_ONLY" };
            static const char *ab[4] = { "Cs", "Cd", "0", "?" };
            static const char *cc[4] = { "As", "Ad", "FIX", "?" };
        {
            static const char *zf[16] = {
                "Z32","Z24","Z16","?","?","?","?","?","?","?","Z16S",
                "?","?","?","?","?" };
            ps2_log("      zbuf zbp=%-8u psm=%s", drawc[i].zbp,
                    zf[drawc[i].zpsm & 15u]);
        }
        ps2_log("      test afail=%s zte=%u | blend abe=%u "
                    "(%s-%s)*%s+%s fix=%u colclamp=%u", af[drawc[i].afail & 3u],
                    drawc[i].zte, drawc[i].abe2,
                    ab[drawc[i].ba & 3u], ab[drawc[i].bb & 3u],
                    cc[drawc[i].bc & 3u], ab[drawc[i].bd & 3u],
                    drawc[i].bfix, drawc[i].colclamp);
        }
        ps2_log("      tex tbp=%-8u psm=%-3u texrt=%u  xy [%.1f,%.1f]..[%.1f,%.1f]"
                "  z [%.6f,%.6f]  rgb [%.0f,%.0f] a [%.0f,%.0f]  fst=%d "
                "st [%g,%g]..[%g,%g]",
                drawc[i].tbp, drawc[i].tpsm, drawc[i].texrt,
                drawc[i].x0, drawc[i].y0, drawc[i].x1, drawc[i].y1,
                (double)drawc[i].z0, (double)drawc[i].z1,
                drawc[i].cmin, drawc[i].cmax, drawc[i].amin, drawc[i].amax,
                drawc[i].fst, (double)drawc[i].s0, (double)drawc[i].t0_,
                (double)drawc[i].s1, (double)drawc[i].t1);
        if (drawc[i].tme) {
            static const char *wm[4] = { "REPEAT", "CLAMP", "REGION_CLAMP",
                                         "REGION_REPEAT" };
            ps2_log("      clamp wms=%s wmt=%s u [%u,%u] v [%u,%u]",
                    wm[drawc[i].wms & 3u], wm[drawc[i].wmt & 3u],
                    drawc[i].minu, drawc[i].maxu,
                    drawc[i].minv, drawc[i].maxv);
            {   u64 t1 = drawc[i].tex1;
                s32 k = (s32)((t1 >> 32) & 0xFFFull);
                if (k & 0x800) k -= 0x1000;
                ps2_log("      tex1 lcm=%u mxl=%u mmag=%u mmin=%u mtba=%u L=%u "
                        "K=%.2f  q [%g,%g]  uv [%.3f,%.3f]..[%.3f,%.3f]",
                        (u32)(t1 & 1u), (u32)((t1 >> 2) & 7u),
                        (u32)((t1 >> 5) & 1u), (u32)((t1 >> 6) & 7u),
                        (u32)((t1 >> 9) & 1u), (u32)((t1 >> 19) & 3u),
                        (double)k / 16.0, (double)drawc[i].q0,
                        (double)drawc[i].q1, (double)drawc[i].u0,
                        (double)drawc[i].v0, (double)drawc[i].u1,
                        (double)drawc[i].v1);
            }
        }
    }
    ps2_gs_vertex_hist_report();
    ps2_gs_blit_log_report();
}

static void vhist_add(unsigned path, float x, float y,
                      float sx0, float sx1, float sy0, float sy1) {
    float dx = 0.0f, dy = 0.0f, d;
    unsigned b;
    if (path > 3u) path = 0u;
    vhist[path].total++;
    vhist[path].sx0 = sx0; vhist[path].sx1 = sx1;
    vhist[path].sy0 = sy0; vhist[path].sy1 = sy1;
    {   u64 off = xyoff_reg();
        vhist[path].ofx = (float)(off & 0xFFFFull) / 16.0f;
        vhist[path].ofy = (float)((off >> 32) & 0xFFFFull) / 16.0f; }
    if (vhist[path].ns < VH_SAMPLES) {
        vhist[path].sx[vhist[path].ns] = x;
        vhist[path].sy[vhist[path].ns] = y;
        vhist[path].ns++;
    }
    if (x < sx0) dx = sx0 - x; else if (x > sx1) dx = x - sx1;
    if (y < sy0) dy = sy0 - y; else if (y > sy1) dy = y - sy1;
    d = dx > dy ? dx : dy;
    if (!(d > 0.0f)) {
        vhist[path].inside++;
        vhist[path].bin[0]++;
        return;
    }
    if (d > vhist[path].worst) vhist[path].worst = d;
    for (b = 1; b < VH_BINS - 1u && d >= (float)(1u << b); b++) { }
    vhist[path].bin[b]++;
}

void ps2_gs_vertex_hist_report(void) {
    static const char *nm[4] = { "unknown path", "PATH1 (VU1 XGKICK)",
                                 "PATH2 (VIF1 DIRECT)", "PATH3 (GIF DMA)" };
    unsigned p, b;
    for (p = 0; p < 4u; p++) {
        char line[512];
        int off = 0;
        if (!vhist[p].total) continue;
        ps2_log("vertex placement, %s: %llu vertices, %.1f%% inside the "
                "scissor box, worst %.0f px outside", nm[p],
                (unsigned long long)vhist[p].total,
                100.0 * (double)vhist[p].inside / (double)vhist[p].total,
                vhist[p].worst);
        ps2_log("   judged against scissor x %.0f..%.0f  y %.0f..%.0f, "
                "XYOFFSET (%.1f,%.1f)", vhist[p].sx0, vhist[p].sx1,
                vhist[p].sy0, vhist[p].sy1, vhist[p].ofx, vhist[p].ofy);
        {
            char sm[256];
            int so = 0;
            unsigned k;
            for (k = 0; k < vhist[p].ns && so < (int)sizeof sm - 40; k++)
                so += snprintf(sm + so, sizeof sm - (size_t)so,
                               " (%.1f,%.1f)->raw(%.0f,%.0f)",
                               vhist[p].sx[k], vhist[p].sy[k],
                               (vhist[p].sx[k] + vhist[p].ofx) * 16.0f,
                               (vhist[p].sy[k] + vhist[p].ofy) * 16.0f);
            ps2_log("   first vertices, window->raw:%s", sm);
        }
        for (b = 0; b < VH_BINS && off < (int)sizeof line - 40; b++) {
            if (!vhist[p].bin[b]) continue;
            if (b == 0)
                off += snprintf(line + off, sizeof line - (size_t)off,
                                "  inside=%llu",
                                (unsigned long long)vhist[p].bin[0]);
            else if (b == VH_BINS - 1u)
                off += snprintf(line + off, sizeof line - (size_t)off,
                                "  >=%u:%llu", 1u << (b - 1u),
                                (unsigned long long)vhist[p].bin[b]);
            else
                off += snprintf(line + off, sizeof line - (size_t)off,
                                "  <%u:%llu", 1u << b,
                                (unsigned long long)vhist[p].bin[b]);
        }
        ps2_log("   px outside:%s", line);
    }
}

static void draw_census(const ps2_vk_state *st, const ps2_vk_vertex *v, int n) {
    unsigned i;
    int k;
    if (!ps2_diag_armed) return;
    for (i = 0; i < drawc_n; i++)
        if (drawc[i].path == (u32)ps2_gif_path
            && drawc[i].prim == (gs_prim & 7u) && drawc[i].tme == (u32)st->tme
            && drawc[i].ate == (u32)st->ate && drawc[i].atst == (u32)st->atst
            && drawc[i].aref == st->aref && drawc[i].ztst == st->ztst
            && drawc[i].zwrite == (u32)st->zwrite && drawc[i].abe == (u32)st->abe
            && drawc[i].tfx == st->tfx && drawc[i].fbp == fb_base()
            && drawc[i].wms == st->wms && drawc[i].wmt == st->wmt
            && drawc[i].minu == st->minu && drawc[i].maxu == st->maxu
            && drawc[i].minv == st->minv && drawc[i].maxv == st->maxv
            && drawc[i].tbp == (u32)(tex0_reg() & 0x3FFFull) * 256u) break;
    if (i == drawc_n) {
        if (drawc_n >= DRAWCENSUS_N) { drawc_dropped++; return; }
        drawc_n++;
        drawc[i].path = (u32)ps2_gif_path;
        drawc[i].first_seq = ps2_gs_prim_seq;
        drawc[i].prim = gs_prim & 7u; drawc[i].tme = st->tme;
        drawc[i].ate = st->ate; drawc[i].atst = st->atst;
        drawc[i].aref = st->aref; drawc[i].ztst = st->ztst;
        drawc[i].zwrite = st->zwrite; drawc[i].abe = st->abe;
        drawc[i].tfx = st->tfx;
        drawc[i].fbp = fb_base();
        drawc[i].tbp = (u32)(tex0_reg() & 0x3FFFull) * 256u;
        drawc[i].x0 = drawc[i].y0 = 1e9f;
        drawc[i].x1 = drawc[i].y1 = -1e9f;
        drawc[i].z0 = drawc[i].cmin = drawc[i].amin = 1e9f;
        drawc[i].z1 = drawc[i].cmax = drawc[i].amax = -1e9f;
        drawc[i].s0 = drawc[i].t0_ = 1e9f;
        drawc[i].s1 = drawc[i].t1 = -1e9f;
        drawc[i].q0 = drawc[i].u0 = drawc[i].v0 = 1e9f;
        drawc[i].q1 = drawc[i].u1 = drawc[i].v1 = -1e9f;
    }
    drawc[i].n++;
    drawc[i].last_seq = ps2_gs_prim_seq;
    drawc_seq++;
    drawc[i].fbpsm = st->fb_psm; drawc[i].fbw = st->fb_w; drawc[i].fbh = st->fb_h;
    drawc[i].fbp = fb_base(); drawc[i].zbp = zb_base();
    drawc[i].zpsm = (u32)((zbuf_reg() >> 24) & 0xFull);
    drawc[i].tbp = (u32)(tex0_reg() & 0x3FFFull) * 256u;
    drawc[i].tpsm = (u32)((tex0_reg() >> 20) & 0x3Full);
    drawc[i].tcc = (u32)st->tcc;
    drawc[i].afail = st->afail; drawc[i].zte = st->zte;
    drawc[i].ba = st->alpha_a; drawc[i].bb = st->alpha_b;
    drawc[i].bc = st->alpha_c; drawc[i].bd = st->alpha_d;
    drawc[i].bfix = st->alpha_fix; drawc[i].abe2 = (u32)st->abe;
    drawc[i].colclamp = (u32)(gs_reg[GS_COLCLAMP] & 1ull);
    drawc[i].wms = st->wms; drawc[i].wmt = st->wmt;
    drawc[i].minu = st->minu; drawc[i].maxu = st->maxu;
    drawc[i].minv = st->minv; drawc[i].maxv = st->maxv;
    drawc[i].rt = st->rt; drawc[i].texrt = st->tex_rt;
    drawc[i].fst = st->fst;
    drawc[i].tex1 = tex1_reg();
    for (k = 0; k < 4; k++) drawc[i].sc[k] = st->scissor[k];
    for (k = 0; k < n; k++) {
        float c;
        vhist_add((unsigned)ps2_gif_path, v[k].x, v[k].y,
                  (float)st->scissor[0], (float)st->scissor[1],
                  (float)st->scissor[2], (float)st->scissor[3]);
        if (v[k].x < drawc[i].x0) drawc[i].x0 = v[k].x;
        if (v[k].x > drawc[i].x1) drawc[i].x1 = v[k].x;
        if (v[k].y < drawc[i].y0) drawc[i].y0 = v[k].y;
        if (v[k].y > drawc[i].y1) drawc[i].y1 = v[k].y;
        if (v[k].z < drawc[i].z0) drawc[i].z0 = v[k].z;
        if (v[k].z > drawc[i].z1) drawc[i].z1 = v[k].z;
        c = v[k].r > v[k].g ? v[k].r : v[k].g;
        if (v[k].b > c) c = v[k].b;
        if (c < drawc[i].cmin) drawc[i].cmin = c;
        if (c > drawc[i].cmax) drawc[i].cmax = c;
        if (v[k].a < drawc[i].amin) drawc[i].amin = v[k].a;
        if (v[k].a > drawc[i].amax) drawc[i].amax = v[k].a;
        if (v[k].s < drawc[i].s0) drawc[i].s0 = v[k].s;
        if (v[k].s > drawc[i].s1) drawc[i].s1 = v[k].s;
        if (v[k].t < drawc[i].t0_) drawc[i].t0_ = v[k].t;
        if (v[k].t > drawc[i].t1) drawc[i].t1 = v[k].t;
        if (v[k].q < drawc[i].q0) drawc[i].q0 = v[k].q;
        if (v[k].q > drawc[i].q1) drawc[i].q1 = v[k].q;
        if (st->tme && v[k].q != 0.0f) {
            float u = st->fst ? v[k].s : v[k].s / v[k].q * st->tex_w;
            float w = st->fst ? v[k].t : v[k].t / v[k].q * st->tex_h;
            if (u < drawc[i].u0) drawc[i].u0 = u;
            if (u > drawc[i].u1) drawc[i].u1 = u;
            if (w < drawc[i].v0) drawc[i].v0 = w;
            if (w > drawc[i].v1) drawc[i].v1 = w;
        }
    }
}

static void vk_vertex(ps2_vk_vertex *o, const gs_vtx *v, int iip,
                      const gs_vtx *flat, int fst, u32 lod) {
    const gs_vtx *c = iip ? v : flat;
    if (v->nat) {
        o->x = v->fx + 0.5f;
        o->y = v->fy + 0.5f;
        o->z = v->fzn;
    } else {
        o->x = (float)v->x / 16.0f + 0.5f;
        o->y = (float)v->y / 16.0f + 0.5f;
        o->z = z_norm(v->z);
    }
    o->r = (float)((c->rgba >> 0) & 0xFF);
    o->g = (float)((c->rgba >> 8) & 0xFF);
    o->b = (float)((c->rgba >> 16) & 0xFF);
    o->a = (float)((c->rgba >> 24) & 0xFF);
    if (fst) { float m = 16.0f * (float)(1u << lod);
               o->s = (float)v->u / m; o->t = (float)v->v / m; o->q = 1.0f; }
    else     { o->s = v->s; o->t = v->t; o->q = v->q; }
    o->fog = v->fog;
    o->round_uv = 0;
}

static u32 sprite_round_axis(s32 p0, s32 p1, u32 t0, u32 t1) {
    s32 dp = p1 - p0, dt = (s32)t1 - (s32)t0;
    u32 a = (u32)abs(dp), b = (u32)abs(dt), span = a;
    if (!a || ((p0 | p1 | (s32)t0 | (s32)t1) & 7)) return 0;
    while (b) { u32 r = a % b; a = b; b = r; }
    if (span / a >= 32u) return 0;
    return dt <= 0 || !(span & (span - 1u)) ? 1u : 2u;
}

static void vk_triangle(const gs_vtx *a, const gs_vtx *b, const gs_vtx *c) {
    ps2_vk_state st;
    ps2_vk_vertex v[3];
    int iip = (gs_prim & PRIM_IIP) ? 1 : 0;
    fill_state(&st);
    vk_vertex(&v[0], a, iip, c, st.fst, st.tex_lod);
    vk_vertex(&v[1], b, iip, c, st.fst, st.tex_lod);
    vk_vertex(&v[2], c, iip, c, st.fst, st.tex_lod);
    draw_census(&st, v, 3);
    {
        int claimed = rn_claim_prim(&st, RN_PRIM_TRI, v, 3);
        rn_census_prim(&st, RN_PRIM_TRI, v, 3, claimed);
        if (!claimed) ps2_vk_draw(PS2_VK_TRIANGLES, &st, v, 3);
    }
    gs_stat_prims++;
}

static struct { u64 native, fallback, verified, mismatched; int shown; } st_uprec;
enum { UPF_PSM, UPF_NOREC, UPF_STALE, UPF_NOCLUT, UPF_CLUTSTALE, UPF_N };
static u64 st_upfail[UPF_N];

void ps2_gs_native_tex_report(void) {
    if (st_uprec.native || st_uprec.fallback)
        ps2_log("rn: native textures -- %llu bindings decoded from the upload records, "
                "%llu from the GS decode; verified %llu identical, %llu different; "
                "%u records, %llu evicted",
                (unsigned long long)st_uprec.native, (unsigned long long)st_uprec.fallback,
                (unsigned long long)st_uprec.verified, (unsigned long long)st_uprec.mismatched,
                uprec_n, (unsigned long long)uprec_evicted);
    if (st_uprec.fallback)
        ps2_log("rn:    not from the records -- format %llu, no transfer of that picture "
                "%llu, written over %llu, no palette transfer %llu, palette written over %llu",
                (unsigned long long)st_upfail[UPF_PSM], (unsigned long long)st_upfail[UPF_NOREC],
                (unsigned long long)st_upfail[UPF_STALE], (unsigned long long)st_upfail[UPF_NOCLUT],
                (unsigned long long)st_upfail[UPF_CLUTSTALE]);
}

typedef struct {
    uprec *r, *pr;
    u32 x0, y0, w, h, bits, psm, cpsm;
    int four, indexed;
} uprec_sel;

static int uprec_pick(u32 x0, u32 y0, u32 w, u32 h, uprec_sel *p, u32 *hash_out) {
    u64 t0 = tex0_reg();
    u32 tbp = (u32)(t0 & 0x3FFFull) * 256u;
    u32 tbw = (u32)((t0 >> 14) & 0x3Full) * 64u;
    u32 psm = (u32)((t0 >> 20) & 0x3Full);
    u32 cbp = (u32)((t0 >> 37) & 0x3FFFull) * 256u;
    u32 cpsm = (u32)((t0 >> 51) & 0xFull);
    u32 csm = (u32)((t0 >> 55) & 1ull);
    u32 bits = psm_upload_bits(psm);
    int four = psm == PSM_T4 || psm == PSM_T4HL || psm == PSM_T4HH;
    int indexed = four || psm == PSM_T8 || psm == PSM_T8H;
    uprec *r, *pr = NULL;
    u32 hash;
    if (!bits || csm || !tbw) {
        static int shown = -1;
        if (shown < 0) shown = getenv("PS2_RN_TEX_VERIFY") ? 0 : 6;
        if (shown++ < 6)
            ps2_log("rn-tex: format not decoded from records: psm %u csm %u tbw %u cpsm %u "
                    "(TEX0 %016llX)", psm, csm, tbw, cpsm, (unsigned long long)t0);
        st_upfail[UPF_PSM]++;
        return 0;
    }
    r = uprec_find(tbp, tbw, psm, x0 + w, y0 + h);
    if (!r) {
        static int shown = -1;
        st_upfail[UPF_NOREC]++;
        if (shown < 0) shown = getenv("PS2_RN_TEX_VERIFY") ? 0 : 8;
        if (shown < 8) {
            shown++;
            ps2_log("rn-tex: no transfer of tbp %u tbw %u psm %u window %u,%u %ux%u; "
                    "its first block is owned by record id %u; transfers owning it:",
                    tbp, tbw, psm, x0, y0, w, h, blk_owner[tbp / 256u]);
            for (u32 i = 0; i < uprec_n; i++)
                if (uprecs[i].id == blk_owner[tbp / 256u])
                    ps2_log("rn-tex:    dbp %u dbw %u psm %u at %u,%u %ux%u valid %d",
                            uprecs[i].dbp, uprecs[i].dbw, uprecs[i].dpsm, uprecs[i].x,
                            uprecs[i].y, uprecs[i].w, uprecs[i].h, uprecs[i].valid);
        }
        return 0;
    }
    if (!uprec_current(r)) {
        st_upfail[UPF_STALE]++;
        return 0;
    }
    hash = uprec_hash(r) ^ (x0 * 73856093u) ^ (y0 * 19349663u) ^ (w * 83492791u) ^ h;
    if (indexed) {
        u32 pw = four ? 8u : 16u, ph = four ? 2u : 16u;
        if (cpsm != PSM_CT32 && cpsm != PSM_CT16 && cpsm != PSM_CT16S) {
            st_upfail[UPF_PSM]++;
            return 0;
        }
        pr = uprec_find(cbp, 0, cpsm, pw, ph);
        if (!pr) { st_upfail[UPF_NOCLUT]++; return 0; }
        if (!uprec_current(pr)) {
            st_upfail[UPF_CLUTSTALE]++;
            return 0;
        }
        hash = (hash * 16777619u) ^ uprec_hash(pr);
    } else if (psm != PSM_CT32) {
        hash = (hash * 16777619u) ^ (u32)(gs_reg[GS_TEXA] ^ (gs_reg[GS_TEXA] >> 32));
    }
    p->r = r; p->pr = pr;
    p->x0 = x0; p->y0 = y0; p->w = w; p->h = h;
    p->bits = bits; p->psm = psm; p->cpsm = cpsm;
    p->four = four; p->indexed = indexed;
    *hash_out = hash;
    return 1;
}

static void uprec_fill(const uprec_sel *p, u8 *out) {
    const uprec *r = p->r, *pr = p->pr;
    u32 pal[256];
    if (p->indexed) {
        u32 n = p->four ? 16u : 256u;
        u32 pw = p->four ? 8u : 16u;
        for (u32 e = 0; e < n; e++) {
            u32 q = p->four ? e : ((e & ~0x18u) | ((e & 0x08u) << 1) | ((e & 0x10u) >> 1));
            u32 cx = q % pw, cy = q / pw, c;
            if (p->cpsm == PSM_CT32) {
                memcpy(&c, pr->bytes + ((size_t)cy * pr->w + cx) * 4u, 4);
            } else {
                u16 raw16;
                u32 rr, gg, bb, aa;
                memcpy(&raw16, pr->bytes + ((size_t)cy * pr->w + cx) * 2u, 2);
                rr = (raw16 & 0x1Fu) << 3;
                gg = ((raw16 >> 5) & 0x1Fu) << 3;
                bb = ((raw16 >> 10) & 0x1Fu) << 3;
                aa = (raw16 >> 15) & 1u ? 0x80u : 0u;
                c = rr | (gg << 8) | (bb << 16) | (aa << 24);
            }
            {
                u32 a = (c >> 24) & 0xFFu;
                a = a >= 128u ? 255u : a * 2u;
                pal[e] = (c & 0x00FFFFFFu) | (a << 24);
            }
        }
    }
    for (u32 y = 0; y < p->h; y++) {
        u8 *row = out + (size_t)y * p->w * 4u;
        for (u32 x = 0; x < p->w; x++) {
            u32 sx = p->x0 + x, sy = p->y0 + y, raw, px;
            size_t k = (size_t)sy * r->w + sx;
            switch (p->bits) {
            case 4:  raw = (r->bytes[k >> 1] >> ((k & 1u) * 4u)) & 0xFu; break;
            case 8:  raw = r->bytes[k]; break;
            case 16: { u16 v; memcpy(&v, r->bytes + k * 2u, 2); raw = v; break; }
            case 24: raw = (u32)r->bytes[k * 3u] | ((u32)r->bytes[k * 3u + 1u] << 8)
                         | ((u32)r->bytes[k * 3u + 2u] << 16); break;
            default: memcpy(&raw, r->bytes + k * 4u, 4); break;
            }
            px = p->indexed ? pal[raw & 0xFFu] : texel_rgba(raw, p->psm);
            memcpy(row + x * 4u, &px, 4);
        }
    }
}

static void native_state_decode(ps2_vk_state *st) {
    st->tex_index = current_texture(&st->tex_w, &st->tex_h);
    if (st->tex_index == 0xFFFFFFFFu) st->tme = 0;
}

void ps2_gs_native_state(ps2_vk_state *st) {
    static int sw = -1, verify = -1;
    if (sw < 0) {
        const char *e = getenv("PS2_RN_TEX");
        sw = !(e && *e == '0');
        e = getenv("PS2_RN_TEX_VERIFY");
        verify = e && *e && *e != '0';
    }
    fill_defer_tex = sw && !verify;
    fill_state(st);
    fill_defer_tex = 0;
    if (sw && st->tme && !st->tex_rt && st->tex_index != 0xFFFFFFFFu && st->tex_lod == 0u
        && ps2_vk_enabled() && ps2_vk_mesh_material_ok(st)) {
        u32 iw, ih, x0 = 0, y0 = 0, w, h, idx;
        if (st->tex_index == GS_TEX_DEFERRED) { iw = fill_defer_w; ih = fill_defer_h; }
        else ps2_vk_texture_size(st->tex_index, &iw, &ih);
        w = iw;
        h = ih;
        if (st->wms == 2u && st->minu <= st->maxu) { x0 = st->minu; w = st->maxu - st->minu + 1u; }
        if (st->wmt == 2u && st->minv <= st->maxv) { y0 = st->minv; h = st->maxv - st->minv + 1u; }
        if (x0 >= iw || y0 >= ih) {
            if (st->tex_index == GS_TEX_DEFERRED) native_state_decode(st);
            return;
        }
        if (w > iw - x0) w = iw - x0;
        if (h > ih - y0) h = ih - y0;
        idx = 0xFFFFFFFFu;
        {
            static u8 *buf;
            static size_t cap;
            u32 hash;
            if ((size_t)w * h * 4u > cap) {
                u8 *nb = (u8 *)realloc(buf, (size_t)w * h * 4u);
                if (nb) { buf = nb; cap = (size_t)w * h * 4u; }
            }
            uprec_sel sel;
            if ((size_t)w * h * 4u <= cap && uprec_pick(x0, y0, w, h, &sel, &hash)) {
                u64 key = ((u64)hash << 20) ^ ((u64)w << 10) ^ h;
                idx = verify ? 0xFFFFFFFFu : ps2_vk_texture_native_find(key, hash, w, h);
                if (idx == 0xFFFFFFFFu) uprec_fill(&sel, buf);
                if (verify) {
                    const u8 *src = ps2_vk_texture_pixels(st->tex_index);
                    int same = src != NULL;
                    for (u32 r = 0; same && r < h; r++)
                        same = !memcmp(buf + (size_t)r * w * 4u,
                                       src + ((size_t)(y0 + r) * iw + x0) * 4u,
                                       (size_t)w * 4u);
                    if (same) st_uprec.verified++;
                    else {
                        st_uprec.mismatched++;
                        if (st_uprec.shown++ < 8)
                            ps2_log("rn-tex: upload records disagree with the GS decode: "
                                    "TEX0 %016llX window %u,%u %ux%u",
                                    (unsigned long long)tex0_reg(), x0, y0, w, h);
                    }
                }
                if (idx == 0xFFFFFFFFu)
                    idx = ps2_vk_texture_native_rgba(key, hash, buf, w, h);
                if (idx != 0xFFFFFFFFu) st_uprec.native++;
            }
        }
        if (idx == 0xFFFFFFFFu) {
            st_uprec.fallback++;
            if (st->tex_index == GS_TEX_DEFERRED) native_state_decode(st);
            if (st->tex_index != 0xFFFFFFFFu)
                idx = ps2_vk_texture_native(st->tex_index, x0, y0, w, h);
        }
        if (idx != 0xFFFFFFFFu) {
            st->tex_index = idx;
            st->native_region = (st->wms == 2u || st->wmt == 2u) ? 1 : 0;
        }
    }
    if (st->tex_index == GS_TEX_DEFERRED) native_state_decode(st);
}

void ps2_gs_native_frame(float *xoff, float *yoff, float *zmax) {
    u64 off = xyoff_reg();
    u32 psm = (u32)((zbuf_reg() >> 24) & 0xFull);
    *xoff = (float)(off & 0xFFFFull) / 16.0f;
    *yoff = (float)((off >> 32) & 0xFFFFull) / 16.0f;
    *zmax = (psm == 1) ? 16777215.0f : (psm >= 2 ? 65535.0f : 4294967295.0f);
}

static void vk_sprite(const gs_vtx *a, const gs_vtx *b) {
    ps2_vk_state st;
    ps2_vk_vertex q[4], t[6];
    int i;
    fill_state(&st);
    vk_vertex(&q[0], a, 0, b, st.fst, st.tex_lod);
    vk_vertex(&q[2], b, 0, b, st.fst, st.tex_lod);
    if (st.tex_self && st.fst && st.fb_psm == PSM_CT16S
        && ((tex0_reg() >> 20) & 63u) == PSM_CT16S
        && st.tfx == 1u && st.tcc && !st.abe && !st.ate && !st.date
        && !st.zwrite && !st.fb_mask && !st.fba
        && st.wms == 3u && st.wmt == 3u && st.minu == 1015u
        && st.maxu == 0u && st.minv == 1023u && st.maxv == 0u
        && ((tex0_reg() >> 14) & 63u) * 64u == st.fb_w
        && (a->x & 1023) == 0 && (a->y & 1023) == 0
        && (b->x & 1023) == 1016 && (b->y & 1023) == 0
        && b->x > a->x && b->y > a->y
        && a->u == a->x + 8 && b->u == b->x + 8
        && a->v == a->y + 8 && b->v == b->y + 8) {
        st.shuffle_rg = 1;
        st.shuffle_alpha = (u32)((gs_reg[GS_TEXA] >> 7) & 1u)
                         | (u32)(((gs_reg[GS_TEXA] >> 39) & 1u) << 1)
                         | (u32)(((gs_reg[GS_TEXA] >> 15) & 1u) << 2);
        q[0].x = (float)a->x / 16.0f;
        q[2].x = ((float)b->x + 8.0f) / 16.0f;
        q[0].y = (float)a->y / 32.0f;
        q[2].y = (float)b->y / 32.0f;
        st.scissor[2] /= 2;
        st.scissor[3] /= 2;
    }
    q[1] = q[0]; q[1].x = q[2].x; q[1].s = q[2].s;
    q[3] = q[2]; q[3].x = q[0].x; q[3].s = q[0].s;
    for (i = 0; i < 4; i++) {
        q[i].z = q[2].z;
        q[i].r = q[2].r; q[i].g = q[2].g; q[i].b = q[2].b; q[i].a = q[2].a;
        q[i].q = q[2].q;
        q[i].fog = q[2].fog;
    }
    if (q[2].x > q[0].x) {
        float x0 = q[0].x, x1 = q[2].x;
        float p0 = ceilf(x0 - 0.5f), p1 = ceilf(x1 - 0.5f);
        if (p1 > p0) {
            float ds = (q[2].s - q[0].s) / (x1 - x0);
            float s0 = q[0].s + (p0 - x0) * ds;
            float s1 = q[2].s + (p1 - x1) * ds;
            q[0].x = q[3].x = p0;  q[0].s = q[3].s = s0;
            q[1].x = q[2].x = p1;  q[1].s = q[2].s = s1;
        }
    }

    if (!st.shuffle_rg && st.tex_rt && st.fst && q[2].y > q[0].y && q[2].x > q[0].x
        && !no_deinterlace && ps2_cfg.deinterlace) {
        float sy = (q[2].t - q[0].t) / (q[2].y - q[0].y);
        float sx = (q[2].s - q[0].s) / (q[2].x - q[0].x);
        if (sy > 1.9f && sy < 2.1f && sx > 0.95f && sx < 1.05f) {
            float off = q[0].t - (q[0].y - 0.5f) * sy;
            if (off > -2.0f && off < 2.0f) {
                int i;
                for (i = 0; i < 4; i++) {
                    q[i].t -= off;
                    q[i].y = (q[i].y - 0.5f) * 2.0f + 0.5f;
                }
                st.scissor[2] *= 2;
                st.scissor[3] = st.scissor[3] * 2 + 1;
                st.fb_h = (u32)st.scissor[3] + 1u;
                gs_stat_deint++;
            }
        }
    }
    t[0] = q[0]; t[1] = q[1]; t[2] = q[2];
    t[3] = q[0]; t[4] = q[2]; t[5] = q[3];
    for (i = 0; i < 6; i++) t[i].round_uv = PS2_VK_RUV_SPRITE;
    if (st.tme && st.fst && st.tex_point && !st.shuffle_rg
        && !st.tex_rt && !st.tex_lod) {
        u32 ru = sprite_round_axis(a->x, b->x, a->u, b->u);
        u32 rv = sprite_round_axis(a->y, b->y, a->v, b->v);
        s32 ox = (a->x & 15) ? -2048 : a->x / 16;
        s32 oy = (a->y & 15) ? -2048 : a->y / 16;
        if (ox >= -2048 && ox < 2048 && oy >= -2048 && oy < 2048) {
            u32 info = ru | (rv << 2) | ((u32)(ox + 2048) << 4)
                       | ((u32)(oy + 2048) << 16) | (1u << 28);
            for (i = 0; i < 6; i++) t[i].round_uv |= info;
        }
    }
    if (st.tex_rt) {
        u64 i = blitlog_n++ % BLITLOG_N;
        blitlog[i].frame = ps2_gs_frame_count;
        blitlog[i].dst = st.rt;
        blitlog[i].src = st.tex_rt - 1u;
        blitlog[i].y0 = q[0].y; blitlog[i].y1 = q[2].y;
        blitlog[i].t0 = q[0].t; blitlog[i].t1 = q[2].t;
        blitlog[i].x0 = q[0].x; blitlog[i].x1 = q[2].x;
        blitlog[i].s0 = q[0].s; blitlog[i].s1 = q[2].s;
    }
    draw_census(&st, t, 6);
    if (rn_claim_prim(&st, RN_PRIM_SPRITE, t, 6)) {
        rn_census_prim(&st, RN_PRIM_SPRITE, t, 3, 1);
        gs_stat_prims++;
        return;
    }
    rn_census_prim(&st, RN_PRIM_SPRITE, t, 3, 0);
    ps2_vk_draw(PS2_VK_TRIANGLES, &st, t, 3);
    ps2_vk_draw(PS2_VK_TRIANGLES, &st, t + 3, 3);
    gs_stat_prims++;
}

static void vk_line(const gs_vtx *a, const gs_vtx *b) {
    ps2_vk_state st;
    ps2_vk_vertex v[2];
    fill_state(&st);
    vk_vertex(&v[0], a, (gs_prim & PRIM_IIP) ? 1 : 0, b, st.fst, st.tex_lod);
    vk_vertex(&v[1], b, (gs_prim & PRIM_IIP) ? 1 : 0, b, st.fst, st.tex_lod);
    draw_census(&st, v, 2);
    {
        int claimed = rn_claim_prim(&st, RN_PRIM_LINE, v, 2);
        rn_census_prim(&st, RN_PRIM_LINE, v, 2, claimed);
        if (!claimed) ps2_vk_draw(PS2_VK_LINES, &st, v, 2);
    }
    gs_stat_prims++;
}

static void vk_point(const gs_vtx *a) {
    ps2_vk_state st;
    ps2_vk_vertex v[1];
    fill_state(&st);
    vk_vertex(&v[0], a, 1, a, st.fst, st.tex_lod);
    draw_census(&st, v, 1);
    {
        int claimed = rn_claim_prim(&st, RN_PRIM_POINT, v, 1);
        rn_census_prim(&st, RN_PRIM_POINT, v, 1, claimed);
        if (!claimed) ps2_vk_draw(PS2_VK_POINTS, &st, v, 1);
    }
    gs_stat_prims++;
}

static void vk_assemble(const gs_vtx *v) {
    PS2_PHASE_BEGIN(PS2_PH_PRIM);
    ps2_gs_prim_seq++;
    switch (gs_prim & 7u) {
    case PRIM_POINT:
        vk_point(v);
        gs_qn = 0;
        break;
    case PRIM_LINE:
        if (gs_qn >= 2) { vk_line(&gs_queue[gs_qn - 2], &gs_queue[gs_qn - 1]);
                          gs_qn = 0; }
        break;
    case PRIM_LINESTRIP:
        if (gs_qn >= 2) {
            vk_line(&gs_queue[gs_qn - 2], &gs_queue[gs_qn - 1]);
            gs_queue[0] = gs_queue[gs_qn - 1];
            gs_qn = 1;
        }
        break;
    case PRIM_TRI:
        if (gs_qn == 3) {
            vk_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]);
            gs_qn = 0;
        }
        break;
    case PRIM_TRISTRIP:
        if (gs_qn == 3) {
            vk_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]);
            gs_queue[0] = gs_queue[1];
            gs_queue[1] = gs_queue[2];
            gs_qn = 2;
        }
        break;
    case PRIM_TRIFAN:
        if (gs_qn == 3) {
            vk_triangle(&gs_queue[0], &gs_queue[1], &gs_queue[2]);
            gs_queue[1] = gs_queue[2];
            gs_qn = 2;
        }
        break;
    case PRIM_SPRITE:
        if (gs_qn == 2) { vk_sprite(&gs_queue[0], &gs_queue[1]); gs_qn = 0; }
        else if (gs_qn == 3) { vk_sprite(&gs_queue[1], &gs_queue[2]); gs_qn = 0; }
        break;
    default:
        gs_qn = 0;
        break;
    }
    PS2_PHASE_END(PS2_PH_PRIM);
}

int ps2_gs_present(void) {
    return ps2_gfxq_field();
}

int ps2_gs_present_now(void) {
    u64 fb;
    fb = dispfb_reg();
    u32 dw = ps2_gs_display_width(), dh = ps2_gs_display_height();
    u32 dbx = (u32)((fb >> 32) & 0x7FFull);
    u32 dby = (u32)((fb >> 43) & 0x7FFull);
    u32 base = (u32)(fb & 0x1FFull) * 2048u * 4u;
    int slot;
    ps2_gs_frame_count++;
    rn_census_frame();
    rn_dump_frame();
    ps2_cap_present();
    ps2_cap_maybe_start();
    if (ps2_statecap_gs_pending()) {
        ps2_gs_statecap(ps2_statecap_dir());
        ps2_statecap_gs_done();
    }
    if (!ps2_vk_enabled()) return !ps2_vk_closed();
    if (!dw || dw > 2048) dw = 640;
    if (!dh || dh > 2048) dh = 448;
    gs_disp_base = base;
    if (PS2_ENV("PS2_TRACE_DISP")) {
        static u64 last, last2;
        u64 fb1 = ps2_gs_priv_read(0x12000070u);
        u64 fb2 = ps2_gs_priv_read(0x12000090u);
        u64 pm  = ps2_gs_priv_read(0x12000000u);
        u64 key = (fb << 8) | (u64)(dw & 0xFFu);
        u64 key2 = fb1 ^ (fb2 * 3ull) ^ (pm << 1);
        if (key != last || key2 != last2) {
            u64 d = display_reg();
            last = key; last2 = key2;
            ps2_log("disp: frame %llu  DISPFB base=%u fbw=%u psm=%u "
                    "dbx=%u dby=%u | DISPLAY dx=%u dy=%u magh=%u magv=%u "
                    "dw=%u dh=%u | shown %ux%u | PMODE=%llX circuit=%d "
                    "DISPFB1=%llX DISPFB2=%llX",
                    (unsigned long long)ps2_gs_frame_count, base,
                    (u32)((fb >> 9) & 0x3Full) * 64u,
                    (u32)((fb >> 15) & 0x1Full), dbx, dby,
                    (u32)(d & 0x7FFull), (u32)((d >> 12) & 0x7FFull),
                    (u32)((d >> 23) & 0xFull) + 1u,
                    (u32)((d >> 27) & 3ull) + 1u,
                    (u32)((d >> 32) & 0xFFFull) + 1u,
                    (u32)((d >> 44) & 0x7FFull) + 1u, dw, dh,
                    (unsigned long long)pm, (pm & 2ull) ? 2 : 1,
                    (unsigned long long)fb1, (unsigned long long)fb2);
        }
    }
    slot = rt_find(base);
    {
        static int keep = -1;
        int stale = slot < 0;
        if (keep < 0) keep = PS2_ENV("PS2_DISPLAY_STALE") ? 1 : 0;
        if (!stale && gs_rt_field[slot] + 2u < ps2_gs_frame_count) {
            u32 s0 = gs_rt_base[slot];
            u32 s1 = s0 + (gs_rt_field_pages[slot] ? gs_rt_field_pages[slot] : 1u) * GS_PAGE_BYTES;
            for (u32 i = 0; i < gs_rt_n && !stale; i++) {
                u32 t0, t1;
                if ((int)i == slot || gs_rt_epoch[i] <= gs_rt_epoch[slot]) continue;
                t0 = gs_rt_base[i];
                t1 = t0 + (gs_rt_field_pages[i] ? gs_rt_field_pages[i] : 1u) * GS_PAGE_BYTES;
                stale = t0 < s1 && s0 < t1;
            }
        }
        if (stale && !keep) {
            static u32 said;
            if (said++ < 8u)
                ps2_log("gs: field %u shows base %u, which holds nothing current "
                        "(%s) -- black", ps2_gs_frame_count, base,
                        slot < 0 ? "never a target" : "written over since it was drawn");
            return ps2_vk_present(dbx, dby, dw, dh, PS2_VK_PRESENT_BLANK);
        }
    }
    {
        static int tp = -1;
        if (tp < 0) tp = PS2_ENV("PS2_TRACE_PRESENT") ? 1 : 0;
        if (tp && (slot < 0 || gs_rt_field[slot] + 2u < ps2_gs_frame_count)) {
            static u32 shown;
            if (shown++ < 400u)
                ps2_log("present: field %u DISPFB base %u -> %s%d, last drawn field %u",
                        ps2_gs_frame_count, base, slot < 0 ? "no slot, showing slot " : "slot ",
                        slot < 0 ? 0 : slot, slot < 0 ? gs_rt_field[0] : gs_rt_field[slot]);
        }
    }
    return ps2_vk_present(dbx, dby, dw, dh, slot < 0 ? 0u : (u32)slot);
}

void ps2_gs_write_reg(u32 reg, u64 val) {
    if (g_cap_shallow) ps2_cap_reg(reg, val);
    if (PS2_UNLIKELY(rn_dump_on)) rn_dump_gsreg(reg, val);
    gs_stat_regs++;
    if (reg < 0x64) {
        gs_reg_hist[reg]++;
        if (gs_reg[reg] != val && !gs_reg_is_data[reg]) gs_state_ver++;
        gs_reg[reg] = val;
    }
    switch (reg) {
    case GS_PRIM:
        gs_qn = 0;
        gs_strip_parity = 0;
        __attribute__((fallthrough));
    case GS_PRMODE:
    case GS_PRMODECONT:
        gs_prim = (u32)(gs_reg[GS_PRIM] & 7u) |
            (u32)(gs_reg[(gs_reg[GS_PRMODECONT] & 1u) ? GS_PRIM : GS_PRMODE] & 0x7F8u);
        break;
    case GS_RGBAQ: {
        u32 q32 = (u32)(val >> 32);
        gs_rgba = (u32)val;
        memcpy(&gs_q, &q32, 4);
        break;
    }
    case GS_ST: {
        u32 s32b = (u32)val, t32 = (u32)(val >> 32);
        memcpy(&gs_st_s, &s32b, 4);
        memcpy(&gs_st_t, &t32, 4);
        break;
    }
    case GS_FOG:
        gs_fog = (float)((val >> 56) & 0xFFull) / 255.0f;
        break;
    case GS_UV:
        gs_u = (u16)(val & 0x3FFFull);
        gs_v = (u16)((val >> 16) & 0x3FFFull);
        break;
    case GS_XYZF2:
        gs_fog = (float)((val >> 56) & 0xFFull) / 255.0f;
        vertex_kick((s32)(val & 0xFFFFull), (s32)((val >> 16) & 0xFFFFull),
                    (u32)((val >> 32) & 0xFFFFFFull), 1);
        break;
    case GS_XYZF3:
        gs_fog = (float)((val >> 56) & 0xFFull) / 255.0f;
        vertex_kick((s32)(val & 0xFFFFull), (s32)((val >> 16) & 0xFFFFull),
                    (u32)((val >> 32) & 0xFFFFFFull), 0);
        break;
    case GS_XYZ2:
        vertex_kick((s32)(val & 0xFFFFull), (s32)((val >> 16) & 0xFFFFull),
                    (u32)((val >> 32) & 0xFFFFFFFFull), 1);
        break;
    case GS_XYZ3:
        vertex_kick((s32)(val & 0xFFFFull), (s32)((val >> 16) & 0xFFFFull),
                    (u32)((val >> 32) & 0xFFFFFFFFull), 0);
        break;
    case GS_TRXDIR:
        gs_trxdir_hist[val & 3ull]++;
        if ((val & 3ull) == 0ull) trx_begin();
        else if ((val & 3ull) == 2ull) trx_local();
        break;
    case GS_HWREG:
        trx_data(val);
        break;
    default:
        break;
    }
}

void ps2_gs_hwreg_pair(u64 lo, u64 hi) {
    if (g_cap_shallow) { ps2_cap_reg(GS_HWREG, lo); ps2_cap_reg(GS_HWREG, hi); }
    gs_stat_regs += 2;
    gs_reg_hist[GS_HWREG] += 2;
    gs_reg[GS_HWREG] = hi;
    PS2_PHASE_BEGIN(PS2_PH_TRX);
    trx_data_inner(lo);
    trx_data_inner(hi);
    PS2_PHASE_END(PS2_PH_TRX);
}

void ps2_gs_hwreg_stream(const void *data, u32 qwc) {
    const u8 *src = (const u8 *)data;
    gs_stat_regs += (u64)qwc * 2u;
    gs_reg_hist[GS_HWREG] += (u64)qwc * 2u;
    PS2_PHASE_BEGIN(PS2_PH_TRX);
    for (u32 i = 0; i < qwc; i++) {
        u64 words[2];
        memcpy(words, src + (size_t)i * 16u, sizeof(words));
        gs_reg[GS_HWREG] = words[1];
        trx_data_inner(words[0]);
        trx_data_inner(words[1]);
    }
    PS2_PHASE_END(PS2_PH_TRX);
}

static u64 display_reg(void) {
    u64 pmode = ps2_gs_priv_read(0x12000000u);
    if (pmode & 2ull) return ps2_gs_priv_read(0x120000A0u);
    return ps2_gs_priv_read(0x12000080u);
}

static u64 dispfb_reg(void) {
    u64 pmode = ps2_gs_priv_read(0x12000000u);
    if (pmode & 2ull) return ps2_gs_priv_read(0x12000090u);
    return ps2_gs_priv_read(0x12000070u);
}

u32 ps2_gs_display_width(void) {
    u64 d = display_reg();
    u32 w = (u32)(((d >> 32) & 0xFFFull) + 1u);
    u32 magh = (u32)(((d >> 23) & 0xFull) + 1u);
    return magh ? w / magh : w;
}

u32 ps2_gs_display_height(void) {
    u64 d = display_reg();
    u32 magv = (u32)(((d >> 27) & 3ull) + 1u);
    u32 h = (u32)(((d >> 44) & 0x7FFull) + 1u);
    return magv ? h / magv : h;
}

const u8 *ps2_gs_framebuffer(u32 *w, u32 *h, u32 *stride) {
    u64 fb = dispfb_reg();
    u32 base = (u32)(fb & 0x1FFull) * 2048u * 4u;
    u32 fbw = (u32)((fb >> 9) & 0x3Full) * 64u;
    u32 dw = ps2_gs_display_width(), dh = ps2_gs_display_height();
    if (!fbw) fbw = 640;
    if (!dw || dw > 2048) dw = 640;
    if (!dh || dh > 2048) dh = 448;
    *w = dw; *h = dh; *stride = fbw * 4u;
    if (!gs_vram || base >= GS_VRAM_SIZE) return NULL;
    return gs_vram + base;
}

void ps2_gs_reg_hist_report(void) {
    char line[900] = {0};
    int n = 0;
    for (u32 r = 0; r < 0x64; r++) {
        if (!gs_reg_hist[r]) continue;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X:%llu",
                      r, (unsigned long long)gs_reg_hist[r]);
        if (n > 800) break;
    }
    ps2_log("gs regs:%s", line);
}

void ps2_gs_trx_stats(u64 *transfers, u64 *pixels) {
    *transfers = gs_stat_trx;
    *pixels = gs_stat_trxpix;
}

void ps2_gs_stats(u64 *prims, u64 *pixels, u64 *regs) {
    *prims = gs_stat_prims;
    *pixels = gs_stat_pixels;
    *regs = gs_stat_regs;
}

static int gs_test_fail;

static void gs_expect(const char *what, u32 got, u32 want) {
    if (got != want) {
        gs_test_fail++;
        ps2_log("  FAIL %-42s got %08X want %08X", what, got, want);
    }
}

int ps2_gs_selftest(void) {
    static const u8 col_r0[8] = { 0, 1, 4, 5, 8, 9, 12, 13 };
    static const u8 col_r1[8] = { 2, 3, 6, 7, 10, 11, 14, 15 };
    u32 cls, x, y, i;

    if (!gs_vram) ps2_gs_init();
    if (!gs_swz_ready) gs_swz_init();
    gs_test_fail = 0;
    ps2_log("---- GS local-memory self-test (GS_Users_Manual.pdf p.162-169) ----");

    for (cls = 0; cls < 4; cls++) {
        u32 bw = (cls == GS_SW32) ? 8u : (cls == GS_SW4) ? 32u : 16u;
        u32 colh = (cls == GS_SW8 || cls == GS_SW4) ? 4u : 2u;
        u32 bits = (cls == GS_SW32) ? 32u : (cls == GS_SW16) ? 16u
                 : (cls == GS_SW8) ? 8u : 4u;
        u32 bh0 = (cls == GS_SW32 || cls == GS_SW16) ? 8u : 16u;
        for (y = 0; y < bh0; y++) {
            u32 colnum = y / colh, cy = y % colh;
            for (x = 0; x < bw; x++) {
                u32 w = (gs_swz[cls][y * bw + x] % 512u) / 32u;
                u32 want = (cy & 1u) ? col_r1[x & 7u] : col_r0[x & 7u];
                char nm[64];
                if (colh == 4u && (((cy >> 1) & 1u) ^ (colnum & 1u))) want ^= 8u;
                snprintf(nm, sizeof nm, "class %u column word (%u,%u)", cls, x, y);
                gs_expect(nm, w, want);
            }
        }
        {
            u32 bh = (cls == GS_SW32 || cls == GS_SW16) ? 8u : 16u;
            u8 seen[2048 / 4];
            u32 n = bw * bh, dup = 0, span = 0;
            memset(seen, 0, sizeof seen);
            for (i = 0; i < n; i++) {
                u32 off = gs_swz[cls][i], k;
                for (k = 0; k < bits / 4u; k++) {
                    u32 slot = off / 4u + k;
                    if (slot >= sizeof seen || seen[slot]) dup++;
                    else seen[slot] = 1;
                }
                if (off + bits > span) span = off + bits;
            }
            {
                char nm[64];
                snprintf(nm, sizeof nm, "class %u block is a bijection", cls);
                gs_expect(nm, dup, 0);
                snprintf(nm, sizeof nm, "class %u block spans 2048 bits", cls);
                gs_expect(nm, span, 2048u);
            }
        }
    }

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            u32 v = 0xAABBCCDDu ^ (x * 0x01010101u) ^ (y * 0x00010001u);
            vram_put(0, 64, x, y, PSM_CT32, v);
            gs_expect("T8H  aliases CT32 bits 31-24",
                      vram_get(0, 64, x, y, PSM_T8H),  (v >> 24) & 0xFFu);
            gs_expect("T4HH aliases CT32 bits 31-28",
                      vram_get(0, 64, x, y, PSM_T4HH), (v >> 28) & 0x0Fu);
            gs_expect("T4HL aliases CT32 bits 27-24",
                      vram_get(0, 64, x, y, PSM_T4HL), (v >> 24) & 0x0Fu);
        }
    }

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            u32 lo = 0x1234u ^ (x * 0x0101u) ^ y, hi = 0xBEEFu ^ (x * 0x0303u);
            vram_put(0, 64, x,      y, PSM_CT16, lo);
            vram_put(0, 64, x + 8u, y, PSM_CT16, hi);
            gs_expect("CT16 low half survives the high write",
                      vram_get(0, 64, x, y, PSM_CT16), lo);
            gs_expect("CT16 pair packs into one CT32 slot",
                      vram_get(0, 64, x, y, PSM_CT32), lo | (hi << 16));
        }
    }

    {
        static const u32 fmts[] = { PSM_CT32, PSM_CT24, PSM_CT16, PSM_CT16S,
                                    PSM_T8, PSM_T4, PSM_Z32, PSM_Z16,
                                    PSM_Z16S, PSM_Z24 };
        for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
            u32 psm = fmts[i], bits = psm_bits(psm);
            u32 mask = (bits >= 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            u32 w = 128, h = 64, bad = 0;
            if (psm == PSM_CT24 || psm == PSM_Z24) mask = 0x00FFFFFFu;
            memset(gs_vram, 0, GS_VRAM_SIZE);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    vram_put(0, w, x, y, psm,
                             (x * 2654435761u + y * 40503u) & mask);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    if (vram_get(0, w, x, y, psm)
                        != ((x * 2654435761u + y * 40503u) & mask)) bad++;
            {
                char nm[64];
                snprintf(nm, sizeof nm, "psm %u round-trips %ux%u", psm, w, h);
                gs_expect(nm, bad, 0);
            }
        }
        memset(gs_vram, 0, GS_VRAM_SIZE);
    }

    {
        float w, h;
        const u32 base=0x200000;
        gs_prim=PRIM_TME;
        gs_state_ver++;
        gs_reg[GS_TEX0_1]=(base/256u) | (8ull<<14) | ((u64)PSM_T8<<20)
                        | (9ull<<26) | (9ull<<30);
        gs_reg[GS_TEX1_1]=0;
        trx_ext_note(base,0,0,128,64);
        memset(texcache,0,sizeof(texcache));
        memset(texmembers,0,sizeof(texmembers));
        current_texture(&w,&h);
        gs_expect("partial atlas S scale",(u32)w,512);
        gs_expect("partial atlas T scale",(u32)h,512);
        current_texture(&w,&h);
        gs_expect("cached atlas T scale",(u32)h,512);
        trx_ext_note(base,0,0,512,384);
        gs_mark_dirty(base,base+8192);
        current_texture(&w,&h);
        gs_expect("growing atlas T scale",(u32)h,512);
        gs_reg[GS_TEX1_1]=1ull | (3ull<<2) | (4ull<<6) | (1ull<<9) | (32ull<<32);
        trx_ext_note(base+327680,0,0,32,16);
        current_texture(&w,&h);
        gs_expect("partial mip S scale",(u32)w,128);
        gs_expect("partial mip T scale",(u32)h,128);
        memset(texcache,0,sizeof(texcache));
        memset(texmembers,0,sizeof(texmembers));
    }
    ps2_log("gs selftest: %s", gs_test_fail ? "FAILED" : "all checks passed");
    return gs_test_fail ? 1 : 0;
}
