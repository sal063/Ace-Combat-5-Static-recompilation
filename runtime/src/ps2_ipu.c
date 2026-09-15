#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPU_BCLR   0u
#define IPU_IDEC   1u
#define IPU_BDEC   2u
#define IPU_VDEC   3u
#define IPU_FDEC   4u
#define IPU_SETIQ  5u
#define IPU_SETVQ  6u
#define IPU_CSC    7u
#define IPU_PACK   8u
#define IPU_SETTH  9u

static u8  *in_buf;
static u64 ipu_in_bytes;
static int ipu_starved;
static int ipu_cmd_active;
static u32  in_cap, in_len;
static u64  in_bit;

static u8  *out_buf;
static u32  out_cap, out_len, out_pos;

static u32 ipu_ctrl;
static u32 ipu_data;
static u32 ipu_cbp;
static int ipu_scd, ipu_ecd;

static u8  iq_intra_raster[64], iq_noni_raster[64];
static u16 vq_clut[16];
static u32 ipu_th0, ipu_th1;
static s32 dc_pred[3];

static u64 stat_cmd[16], stat_mb, stat_err;
static u64 stat_starve, stat_resume;

typedef struct {
    u64 in_bit;
    u32 in_len;
    u32 out_len, out_pos;
    u32 ipu_data, ipu_cbp, ipu_th0, ipu_th1;
    int ipu_scd, ipu_ecd;
    s32 dc_pred[3];
    u8  iq_intra[64], iq_noni[64];
    u16 vq[16];
    u64 stat_cmd[16], stat_mb, stat_err;
} ipu_snap;

static ipu_snap cmd_snap;
static u32 ipu_pending_cmd;
static int ipu_pending;
static int ipu_in_command;
static int ipu_said_eos;
static int ipu_said_err;
static int ipu_said_bp;
static u32 ipu_need_qw;

#define IPU_BP_MAX_QW 18u

static void snap_save(ipu_snap *k) {
    k->in_bit = in_bit; k->in_len = in_len;
    k->out_len = out_len; k->out_pos = out_pos;
    k->ipu_data = ipu_data; k->ipu_cbp = ipu_cbp;
    k->ipu_th0 = ipu_th0; k->ipu_th1 = ipu_th1;
    k->ipu_scd = ipu_scd; k->ipu_ecd = ipu_ecd;
    memcpy(k->dc_pred, dc_pred, sizeof dc_pred);
    memcpy(k->iq_intra, iq_intra_raster, sizeof iq_intra_raster);
    memcpy(k->iq_noni, iq_noni_raster, sizeof iq_noni_raster);
    memcpy(k->vq, vq_clut, sizeof vq_clut);
    memcpy(k->stat_cmd, stat_cmd, sizeof stat_cmd);
    k->stat_mb = stat_mb; k->stat_err = stat_err;
}

static void snap_restore(const ipu_snap *k) {
    in_bit = k->in_bit; in_len = k->in_len;
    out_len = k->out_len; out_pos = k->out_pos;
    ipu_data = k->ipu_data; ipu_cbp = k->ipu_cbp;
    ipu_th0 = k->ipu_th0; ipu_th1 = k->ipu_th1;
    ipu_scd = k->ipu_scd; ipu_ecd = k->ipu_ecd;
    memcpy(dc_pred, k->dc_pred, sizeof dc_pred);
    memcpy(iq_intra_raster, k->iq_intra, sizeof iq_intra_raster);
    memcpy(iq_noni_raster, k->iq_noni, sizeof iq_noni_raster);
    memcpy(vq_clut, k->vq, sizeof vq_clut);
    memcpy(stat_cmd, k->stat_cmd, sizeof stat_cmd);
    stat_mb = k->stat_mb; stat_err = k->stat_err;
}

static void in_reserve(u32 n) {
    u32 cap;
    u8 *p;
    if (in_buf && in_len + n <= in_cap) return;
    cap = in_cap ? in_cap : 1u << 16;
    while (cap < in_len + n) cap *= 2u;
    p = (u8 *)realloc(in_buf, cap);
    if (!p) return;
    in_buf = p;
    in_cap = cap;
}

static void in_compact(void) {
    u32 qw = (u32)(in_bit >> 7);
    u32 drop;
    if (ipu_starved) return;
    if (qw < 8u) return;
    drop = qw * 16u;
    if (drop > in_len) drop = in_len;
    if (in_len - drop > 65536u && qw < 256u) return;
    if (in_buf && in_len > drop)
        memmove(in_buf, in_buf + drop, in_len - drop);
    in_len -= drop;
    in_bit -= (u64)drop * 8u;
}

static u32 bit_avail(void) {
    u64 total = (u64)in_len * 8u;
    return in_bit >= total ? 0u : (u32)(total - in_bit);
}

static u32 bit_peek(u32 n) {
    u32 byte = (u32)(in_bit >> 3);
    u32 sh = (u32)(in_bit & 7u);
    u64 v;
    int i;
    if (!n) return 0;
    v = 0;
    for (i = 0; i < 5; i++)
        v = (v << 8) | (u64)(byte + (u32)i < in_len ? in_buf[byte + (u32)i] : 0u);
    if (ipu_cmd_active && bit_avail() < n) ipu_starved = 1;
    return (u32)((v >> (40u - sh - n)) & ((1ull << n) - 1ull));
}

static void bit_skip(u32 n) { in_bit += n; }
static u32  bit_get(u32 n) { u32 v = bit_peek(n); bit_skip(n); return v; }

static int at_start_code(void) {
    return bit_avail() >= 24u && bit_peek(23) == 0u;
}

static void align_to_start_code(void) {
    u64 save = in_bit;
    u32 i;
    in_bit = (in_bit + 7u) & ~7ull;
    for (i = 0; i < 64u; i++) {
        if (bit_avail() >= 24u && bit_peek(24) == 1u) return;
        in_bit += 8;
    }
    in_bit = save;
}

static void out_reset(void) { out_len = out_pos = 0; }

static void out_push(const void *p, u32 n) {
    if (n > UINT32_MAX - out_len) ps2_fatal("IPU output size overflow");
    if (out_len + n > out_cap) {
        if (!out_cap) out_cap = 1u << 20;
        while (out_cap < out_len + n) {
            if (out_cap > UINT32_MAX / 2u) { out_cap = out_len + n; break; }
            out_cap *= 2u;
        }
        u8 *grown = (u8 *)realloc(out_buf, out_cap);
        if (!grown) ps2_fatal("IPU output allocation failed");
        out_buf = grown;
    }
    memcpy(out_buf + out_len, p, n);
    out_len += n;
}

typedef struct { const char *bits; s16 a; } vlc_str;
typedef struct { u32 code; u8 len; s16 a; } vlc_ent;
typedef struct { vlc_ent *e; unsigned n; u8 maxlen; } vlc_tab;

static vlc_ent *vlc_build(const vlc_str *s, unsigned n) {
    vlc_ent *e = (vlc_ent *)malloc(sizeof(vlc_ent) * n);
    if (!e) ps2_fatal("IPU VLC allocation failed");
    unsigned i;
    for (i = 0; i < n; i++) {
        const char *p = s[i].bits;
        u32 v = 0, len = 0;
        for (; *p; p++) { v = (v << 1) | (u32)(*p - '0'); len++; }
        e[i].code = v;
        e[i].len = (u8)len;
        e[i].a = s[i].a;
    }
    return e;
}

static int vlc_get(const vlc_tab *t, s32 *out) {
    u32 win = bit_peek(t->maxlen);
    unsigned i;
    for (i = 0; i < t->n; i++)
        if ((win >> (t->maxlen - t->e[i].len)) == t->e[i].code) {
            bit_skip(t->e[i].len);
            *out = t->e[i].a;
            return 1;
        }
    return 0;
}

static const vlc_str s_mbai[] = {
    { "1",1 },{ "011",2 },{ "010",3 },{ "0011",4 },{ "0010",5 },
    { "00011",6 },{ "00010",7 },{ "0000111",8 },{ "0000110",9 },
    { "00001011",10 },{ "00001010",11 },{ "00001001",12 },{ "00001000",13 },
    { "00000111",14 },{ "00000110",15 },
    { "0000010111",16 },{ "0000010110",17 },{ "0000010101",18 },
    { "0000010100",19 },{ "0000010011",20 },{ "0000010010",21 },
    { "00000100011",22 },{ "00000100010",23 },{ "00000100001",24 },
    { "00000100000",25 },{ "00000011111",26 },{ "00000011110",27 },
    { "00000011101",28 },{ "00000011100",29 },{ "00000011011",30 },
    { "00000011010",31 },{ "00000011001",32 },{ "00000011000",33 },
    { "00000001111",34 },{ "00000001000",35 },
};

static const vlc_str s_mbt_i[] = { { "1",0x01 },{ "01",0x11 } };
static const vlc_str s_mbt_p[] = {
    { "1",0x0a },{ "01",0x02 },{ "001",0x08 },{ "00011",0x01 },
    { "00010",0x1a },{ "00001",0x12 },{ "000001",0x11 },
};
static const vlc_str s_mbt_b[] = {
    { "10",0x0c },{ "11",0x0e },{ "010",0x04 },{ "011",0x06 },
    { "0010",0x08 },{ "0011",0x0a },{ "00011",0x01 },{ "00010",0x1e },
    { "000011",0x1a },{ "000010",0x16 },{ "000001",0x11 },
};
static const vlc_str s_mbt_d[] = { { "1",0x01 } };

static const vlc_str s_motion[] = {
    { "1",0 },
    { "010",1 },{ "011",-1 },{ "0010",2 },{ "0011",-2 },
    { "00010",3 },{ "00011",-3 },{ "0000110",4 },{ "0000111",-4 },
    { "00001010",5 },{ "00001011",-5 },{ "00001000",6 },{ "00001001",-6 },
    { "00000110",7 },{ "00000111",-7 },
    { "0000010110",8 },{ "0000010111",-8 },{ "0000010100",9 },
    { "0000010101",-9 },{ "0000010010",10 },{ "0000010011",-10 },
    { "00000100010",11 },{ "00000100011",-11 },{ "00000100000",12 },
    { "00000100001",-12 },{ "00000011110",13 },{ "00000011111",-13 },
    { "00000011100",14 },{ "00000011101",-14 },{ "00000011010",15 },
    { "00000011011",-15 },{ "00000011000",16 },{ "00000011001",-16 },
};

static const vlc_str s_dmv[] = { { "0",0 },{ "10",1 },{ "11",-1 } };

static const vlc_str s_dc_lum[] = {
    { "100",0 },{ "00",1 },{ "01",2 },{ "101",3 },{ "110",4 },{ "1110",5 },
    { "11110",6 },{ "111110",7 },{ "1111110",8 },{ "11111110",9 },
    { "111111110",10 },{ "111111111",11 },
};
static const vlc_str s_dc_chr[] = {
    { "00",0 },{ "01",1 },{ "10",2 },{ "110",3 },{ "1110",4 },{ "11110",5 },
    { "111110",6 },{ "1111110",7 },{ "11111110",8 },{ "111111110",9 },
    { "1111111110",10 },{ "1111111111",11 },
};

static const vlc_str s_cbp[] = {
    { "111",60 },{ "1101",4 },{ "1100",8 },{ "1011",16 },{ "1010",32 },
    { "10011",12 },{ "10010",48 },{ "10001",20 },{ "10000",40 },
    { "01111",28 },{ "01110",44 },{ "01101",52 },{ "01100",56 },
    { "01011",1 },{ "01010",61 },{ "01001",2 },{ "01000",62 },
    { "001111",24 },{ "001110",36 },{ "001101",3 },{ "001100",63 },
    { "0010111",5 },{ "0010110",9 },{ "0010101",17 },{ "0010100",33 },
    { "0010011",6 },{ "0010010",10 },{ "0010001",18 },{ "0010000",34 },
    { "00011111",7 },{ "00011110",11 },{ "00011101",19 },{ "00011100",35 },
    { "00011011",13 },{ "00011010",49 },{ "00011001",21 },{ "00011000",41 },
    { "00010111",14 },{ "00010110",50 },{ "00010101",22 },{ "00010100",42 },
    { "00010011",15 },{ "00010010",51 },{ "00010001",23 },{ "00010000",43 },
    { "00001111",25 },{ "00001110",37 },{ "00001101",26 },{ "00001100",38 },
    { "00001011",29 },{ "00001010",45 },{ "00001001",53 },{ "00001000",57 },
    { "00000111",30 },{ "00000110",46 },{ "00000101",54 },{ "00000100",58 },
    { "000000111",31 },{ "000000110",47 },{ "000000101",55 },{ "000000100",59 },
    { "000000011",27 },{ "000000010",39 },{ "000000001",0 },
};

typedef struct { s16 run, level; u32 c14; u8 n14; u32 c15; u8 n15; } dct_row;
#include "ps2_ipu_dct.inc"
#define DCT_ESCAPE 111
#define DCT_EOB    112

static vlc_tab t_mbai, t_mbt_i, t_mbt_p, t_mbt_b, t_mbt_d, t_motion, t_dmv;
static vlc_tab t_dc_lum, t_dc_chr, t_cbp;

static s16 dct_lut14[1 << 16], dct_lut15[1 << 16];
static u8  dct_len14[1 << 16], dct_len15[1 << 16];
static int tables_ready;

static void tables_init(void) {
    unsigned i, j;
    if (tables_ready) return;
    t_mbai.e = vlc_build(s_mbai, t_mbai.n = sizeof s_mbai / sizeof *s_mbai);
    t_mbt_i.e = vlc_build(s_mbt_i, t_mbt_i.n = sizeof s_mbt_i / sizeof *s_mbt_i);
    t_mbt_p.e = vlc_build(s_mbt_p, t_mbt_p.n = sizeof s_mbt_p / sizeof *s_mbt_p);
    t_mbt_b.e = vlc_build(s_mbt_b, t_mbt_b.n = sizeof s_mbt_b / sizeof *s_mbt_b);
    t_mbt_d.e = vlc_build(s_mbt_d, t_mbt_d.n = sizeof s_mbt_d / sizeof *s_mbt_d);
    t_motion.e = vlc_build(s_motion, t_motion.n = sizeof s_motion / sizeof *s_motion);
    t_dmv.e = vlc_build(s_dmv, t_dmv.n = sizeof s_dmv / sizeof *s_dmv);
    t_dc_lum.e = vlc_build(s_dc_lum, t_dc_lum.n = sizeof s_dc_lum / sizeof *s_dc_lum);
    t_dc_chr.e = vlc_build(s_dc_chr, t_dc_chr.n = sizeof s_dc_chr / sizeof *s_dc_chr);
    t_cbp.e = vlc_build(s_cbp, t_cbp.n = sizeof s_cbp / sizeof *s_cbp);
    {   vlc_tab *all[10] = { &t_mbai, &t_mbt_i, &t_mbt_p, &t_mbt_b, &t_mbt_d,
                             &t_motion, &t_dmv, &t_dc_lum, &t_dc_chr, &t_cbp };
        for (i = 0; i < 10; i++) {
            u8 m = 0;
            for (j = 0; j < all[i]->n; j++)
                if (all[i]->e[j].len > m) m = all[i]->e[j].len;
            all[i]->maxlen = m;
        }
    }

    for (i = 0; i < (1u << 16); i++) { dct_lut14[i] = -1; dct_lut15[i] = -1; }
    for (i = 0; i < 113; i++) {
        u32 n = dct_rows[i].n14, c = dct_rows[i].c14;
        u32 base = c << (16 - n), span = 1u << (16 - n);
        for (j = 0; j < span; j++) { dct_lut14[base + j] = (s16)i; dct_len14[base + j] = (u8)n; }
        n = dct_rows[i].n15; c = dct_rows[i].c15;
        base = c << (16 - n); span = 1u << (16 - n);
        for (j = 0; j < span; j++) { dct_lut15[base + j] = (s16)i; dct_len15[base + j] = (u8)n; }
    }
    tables_ready = 1;
}

static const u8 scan_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};
static const u8 scan_alt[64] = {
     0, 8,16,24, 1, 9, 2,10, 17,25,32,40,48,56,57,49,
    41,33,26,18, 3,11, 4,12, 19,27,34,42,50,58,35,43,
    51,59,20,28, 5,13, 6,14, 21,29,36,44,52,60,37,45,
    53,61,22,30, 7,15,23,31, 38,46,54,62,39,47,55,63
};
static const u8 qscale_nonlin[32] = {
     0, 1, 2, 3, 4, 5, 6, 7, 8,10,12,14,16,18,20,22,
    24,28,32,36,40,44,48,52,56,64,72,80,88,96,104,112
};

static double idct_c[8][8];

static void idct_init(void) {
    int u, x;
    for (u = 0; u < 8; u++)
        for (x = 0; x < 8; x++)
            idct_c[u][x] = (u ? 1.0 : 0.70710678118654752440)
                         * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
}

static void idct_8x8(const s16 *in, s16 *out, int lo, int hi) {
    double tmp[64];
    int y, x, u, v;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            double s = 0.0;
            for (u = 0; u < 8; u++) if (in[y * 8 + u]) s += idct_c[u][x] * in[y * 8 + u];
            tmp[y * 8 + x] = s * 0.5;
        }
    for (x = 0; x < 8; x++)
        for (y = 0; y < 8; y++) {
            double s = 0.0;
            int r;
            for (v = 0; v < 8; v++) s += idct_c[v][y] * tmp[v * 8 + x];
            s *= 0.5;
            r = (int)(s < 0 ? s - 0.5 : s + 0.5);
            if (r < lo) r = lo;
            if (r > hi) r = hi;
            out[y * 8 + x] = (s16)r;
        }
}

#define IPU_CTRL_RW  0x07FF0000u

#define CTRL_IDP(c)  (((c) >> 16) & 3u)
#define CTRL_AS(c)   (((c) >> 20) & 1u)
#define CTRL_IVF(c)  (((c) >> 21) & 1u)
#define CTRL_QST(c)  (((c) >> 22) & 1u)
#define CTRL_MP1(c)  (((c) >> 23) & 1u)
#define CTRL_PCT(c)  (((c) >> 24) & 7u)

static void dc_pred_reset(void) {
    s32 v = (s32)(128u << CTRL_IDP(ipu_ctrl));
    dc_pred[0] = dc_pred[1] = dc_pred[2] = v;
}

static u32 quant_scale(u32 qsc) {
    if (qsc > 31u) qsc = 31u;
    return CTRL_QST(ipu_ctrl) ? qscale_nonlin[qsc] : qsc * 2u;
}

static int decode_block(s16 *out, int cc, int intra, u32 qsc) {
    s16 coef[64];
    const u8 *scan = CTRL_AS(ipu_ctrl) ? scan_alt : scan_zigzag;
    const u8 *w = intra ? iq_intra_raster : iq_noni_raster;
    u32 qs = quant_scale(qsc);
    int n = 0, sum = 0, i;
    int use15 = intra && CTRL_IVF(ipu_ctrl);
    const s16 *lut = use15 ? dct_lut15 : dct_lut14;
    const u8  *llen = use15 ? dct_len15 : dct_len14;

    memset(coef, 0, sizeof coef);

    if (intra) {
        s32 size = 0, diff = 0;
        if (!vlc_get(cc ? &t_dc_chr : &t_dc_lum, &size)) return 0;
        if (size) {
            u32 v = bit_get((u32)size);
            diff = (v & (1u << (size - 1))) ? (s32)v
                                            : (s32)v - (1 << size) + 1;
        }
        dc_pred[cc] += diff;
        coef[0] = (s16)(dc_pred[cc] * (8 >> CTRL_IDP(ipu_ctrl)));
        sum += coef[0];
        n = 1;
    } else {
        if (bit_peek(1) == 1u) {
            s32 level;
            bit_skip(1);
            level = bit_get(1) ? -1 : 1;
            {
                int p = scan[0];
                int f = ((2 * level + (level < 0 ? -1 : 1)) * w[p] * (int)qs) / 32;
                if (f > 2047) f = 2047;
                if (f < -2048) f = -2048;
                coef[p] = (s16)f;
                sum += f;
            }
            n = 1;
        }
    }

    for (;;) {
        u32 win = bit_peek(16);
        int row = lut[win];
        s32 run, level;
        int p, f;
        if (row < 0) { ipu_ecd = 1; stat_err++; return 0; }
        if (row == DCT_EOB) { bit_skip(llen[win]); break; }
        if (row == DCT_ESCAPE) {
            bit_skip(llen[win]);
            run = (s32)bit_get(6);
            if (CTRL_MP1(ipu_ctrl)) {
                s32 v = (s32)bit_get(8);
                if (v == 0) level = (s32)bit_get(8);
                else if (v == 128) level = (s32)bit_get(8) - 256;
                else level = v >= 128 ? v - 256 : v;
            } else {
                u32 v = bit_get(12);
                level = (v & 0x800u) ? (s32)v - 4096 : (s32)v;
            }
        } else {
            bit_skip(llen[win]);
            run = dct_rows[row].run;
            level = dct_rows[row].level;
            if (bit_get(1)) level = -level;
        }
        n += (int)run;
        if (n > 63) { ipu_ecd = 1; stat_err++; return 0; }
        p = scan[n];
        if (intra)
            f = (level * w[p] * (int)qs * 2) / 32;
        else
            f = ((2 * level + (level < 0 ? -1 : 1)) * w[p] * (int)qs) / 32;
        if (f > 2047) f = 2047;
        if (f < -2048) f = -2048;
        coef[p] = (s16)f;
        sum += f;
        n++;
    }

    if (!CTRL_MP1(ipu_ctrl) && !(sum & 1)) coef[63] ^= 1;

    idct_8x8(coef, out, intra ? 0 : -256, intra ? 255 : 255);
    return 1;
}

static void put_luma(s16 *y, const s16 *blk, int b, int field) {
    int r, c;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            int x = (b & 1) * 8 + c;
            int line = field ? (r * 2 + (b >> 1)) : ((b >> 1) * 8 + r);
            y[line * 16 + x] = blk[r * 8 + c];
        }
}

static int decode_macroblock(s16 *y, s16 *cb, s16 *cr, int intra, u32 cbp,
                             u32 qsc, int field) {
    s16 blk[64];
    int b;
    memset(y, 0, 256 * sizeof(s16));
    memset(cb, 0, 64 * sizeof(s16));
    memset(cr, 0, 64 * sizeof(s16));
    for (b = 0; b < 6; b++) {
        int coded = (cbp >> (5 - b)) & 1u;
        if (!coded) continue;
        if (!decode_block(blk, b < 4 ? 0 : (b == 4 ? 1 : 2), intra, qsc))
            return 0;
        if (b < 4) put_luma(y, blk, b, field);
        else memcpy(b == 4 ? cb : cr, blk, sizeof blk);
    }
    stat_mb++;
    return 1;
}

static const s8 dither[4][4] = {
    { -8,  0, -6,  2 },
    {  4, -4,  6, -2 },
    { -5,  3, -7,  1 },
    {  7, -1,  5, -3 }
};

static u32 csc_pixel(int Y, int Cb, int Cr, int sgn) {
    int Cr1 = Cr - 128, Cb1 = Cb - 128;
    int Cr4 = (0x0cc * Cr1) >> 6;
    int Cr5 = (0x068 * Cr1) >> 6;
    int Cb4 = (0x032 * Cb1) >> 6;
    int Cb5 = (0x102 * Cb1) >> 6;
    int Y3  = (0x095 * (Y - 16)) >> 6;
    int r = (Y3 + Cr4 + 1) >> 1;
    int g = (Y3 - Cb4 - Cr5 + 1) >> 1;
    int b = (Y3 + Cb5 + 1) >> 1;
    int a;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    if (r < (int)ipu_th0 && g < (int)ipu_th0 && b < (int)ipu_th0) {
        r = g = b = 0; a = 0x00;
    } else if (r < (int)ipu_th1 && g < (int)ipu_th1 && b < (int)ipu_th1) {
        a = 0x40;
    } else {
        a = 0x80;
    }
    if (sgn) { r ^= 0x80; g ^= 0x80; b ^= 0x80; }
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

static u16 rgb32_to_rgb16(u32 p, int x, int y, int dte) {
    int r = (int)(p & 0xFFu), g = (int)((p >> 8) & 0xFFu);
    int b = (int)((p >> 16) & 0xFFu), a = (int)((p >> 24) & 0xFFu);
    if (dte) {
        int d = dither[y & 3][x & 3];
        r = (r * 2 + d) >> 1; g = (g * 2 + d) >> 1; b = (b * 2 + d) >> 1;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
    }
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
               | ((a == 0x40) ? 0x8000u : 0u));
}

static u32 vq_index(u16 c) {
    int best = 0, bestd = 1 << 30, i;
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    for (i = 0; i < 16; i++) {
        int cr = vq_clut[i] & 0x1F, cg = (vq_clut[i] >> 5) & 0x1F;
        int cb = (vq_clut[i] >> 10) & 0x1F;
        int dr = r - cr, dg = g - cg, db = b - cb;
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return (u32)best;
}

static void emit_rgb(const s16 *y, const s16 *cb, const s16 *cr,
                     int ofm, int dte, int sgn) {
    u32 rgb[256];
    int j, i;
    for (j = 0; j < 16; j++)
        for (i = 0; i < 16; i++) {
            int c = (j >> 1) * 8 + (i >> 1);
            rgb[j * 16 + i] = csc_pixel(y[j * 16 + i], cb[c], cr[c], sgn);
        }
    if (!ofm) {
        out_push(rgb, sizeof rgb);
    } else {
        u16 p16[256];
        for (j = 0; j < 16; j++)
            for (i = 0; i < 16; i++)
                p16[j * 16 + i] = rgb32_to_rgb16(rgb[j * 16 + i], i, j, dte);
        out_push(p16, sizeof p16);
    }
}

static void emit_raw16(const s16 *y, const s16 *cb, const s16 *cr) {
    {   static int shown;
        if (!shown) {
            shown = 1;
            ps2_log("ipu: bdec out y[0..7]=%d %d %d %d %d %d %d %d  "
                    "cb[0..3]=%d %d %d %d  cr[0..3]=%d %d %d %d",
                    y[0], y[1], y[2], y[3], y[4], y[5], y[6], y[7],
                    cb[0], cb[1], cb[2], cb[3], cr[0], cr[1], cr[2], cr[3]);
        }
    }
    out_push(y, 256 * sizeof(s16));
    out_push(cb, 64 * sizeof(s16));
    out_push(cr, 64 * sizeof(s16));
}

static void cmd_bclr(u32 opt) {
    in_len = 0;
    in_bit = opt & 0x7Fu;
    ipu_scd = ipu_ecd = 0;
}

static void cmd_fdec(u32 fb) {
    bit_skip(fb);
    ipu_data = bit_peek(32);
    in_compact();
}

static void cmd_vdec(u32 tbl, u32 fb) {
    const vlc_tab *t;
    s32 v = 0;
    u64 before;
    bit_skip(fb);
    switch (tbl) {
    case 0: t = &t_mbai; break;
    case 1:
        switch (CTRL_PCT(ipu_ctrl)) {
        case 1:  t = &t_mbt_i; break;
        case 2:  t = &t_mbt_p; break;
        case 3:  t = &t_mbt_b; break;
        default: t = &t_mbt_d; break;
        }
        break;
    case 2: t = &t_motion; break;
    default: t = &t_dmv; break;
    }
    before = in_bit;
    if (!vlc_get(t, &v)) { ipu_ecd = 1; stat_err++; ipu_data = 0; return; }
    ipu_data = ((u32)(in_bit - before) << 16) | ((u32)v & 0xFFFFu);
    in_compact();
}

static void cmd_setiq(u32 opt) {
    u8 *dst = (opt & (1u << 27)) ? iq_noni_raster : iq_intra_raster;
    u8 tmp[64];
    int i;
    bit_skip(opt & 0x3Fu);
    for (i = 0; i < 64; i++) tmp[i] = (u8)bit_get(8);
    for (i = 0; i < 64; i++) dst[scan_zigzag[i]] = tmp[i];
    in_compact();
}

static void cmd_setvq(void) {
    int i;
    for (i = 0; i < 16; i++) {
        u32 lo = bit_get(8), hi = bit_get(8);
        vq_clut[i] = (u16)(lo | (hi << 8));
    }
    in_compact();
}

static void cmd_setth(u32 opt) {
    ipu_th0 = opt & 0x1FFu;
    ipu_th1 = (opt >> 16) & 0x1FFu;
}

static void cmd_bdec(u32 opt) {
    s16 y[256], cb[64], cr[64];
    u32 fb  = opt & 0x3Fu;
    u32 qsc = (opt >> 16) & 0x1Fu;
    int dt  = (int)((opt >> 25) & 1u);
    int dcr = (int)((opt >> 26) & 1u);
    int mbi = (int)((opt >> 27) & 1u);
    u32 cbp = 0x3Fu;
    bit_skip(fb);
    if (dcr) dc_pred_reset();
    if (!mbi) {
        s32 v = 0;
        if (!vlc_get(&t_cbp, &v)) { ipu_ecd = 1; stat_err++; return; }
        cbp = (u32)v;
    }
    ipu_cbp = cbp;
    out_reset();
    if (!decode_macroblock(y, cb, cr, mbi, cbp, qsc, dt)) return;
    emit_raw16(y, cb, cr);
    if (at_start_code()) { ipu_scd = 1; align_to_start_code(); }
    in_compact();
}

static void cmd_idec(u32 opt) {
    s16 y[256], cb[64], cr[64];
    u32 fb  = opt & 0x3Fu;
    u32 qsc = (opt >> 16) & 0x1Fu;
    int dtd = (int)((opt >> 24) & 1u);
    int sgn = (int)((opt >> 25) & 1u);
    int dte = (int)((opt >> 26) & 1u);
    int ofm = (int)((opt >> 27) & 1u);
    int first = 1;

    bit_skip(fb);
    dc_pred_reset();
    out_reset();
    for (;;) {
        s32 mbt = 0;
        int field = 0;
        if (at_start_code()) { ipu_scd = 1; align_to_start_code(); break; }
        if (!first) {
            s32 inc = 0;
            if (!vlc_get(&t_mbai, &inc) || inc != 1) {
                ipu_ecd = 1; stat_err++; break;
            }
        }
        first = 0;
        if (!vlc_get(&t_mbt_i, &mbt)) { ipu_ecd = 1; stat_err++; break; }
        if (dtd) field = (int)bit_get(1);
        if (mbt & 0x10) qsc = bit_get(5);
        if (ipu_starved) break;
        if (!decode_macroblock(y, cb, cr, 1, 0x3Fu, qsc, field)) break;
        emit_rgb(y, cb, cr, ofm, dte, sgn);
        if (ipu_starved) break;
        if (bit_avail() < 32u) { ipu_scd = 1; break; }
    }
    in_compact();
}

static void cmd_csc(u32 opt) {
    u32 mbc = opt & 0x7FFu;
    int dte = (int)((opt >> 26) & 1u);
    int ofm = (int)((opt >> 27) & 1u);
    u32 m;
    if (bit_avail() < mbc * (24u * 128u)) {
        ipu_starved = 1;
        ipu_need_qw = (mbc * (24u * 128u) - bit_avail() + 127u) / 128u;
        return;
    }
    out_reset();
    for (m = 0; m < mbc; m++) {
        s16 y[256], cb[64], cr[64];
        int i;
        for (i = 0; i < 256; i++) y[i] = (s16)bit_get(8);
        for (i = 0; i < 64; i++) cb[i] = (s16)bit_get(8);
        for (i = 0; i < 64; i++) cr[i] = (s16)bit_get(8);
        {   static int shown;
            if (!shown) {
                shown = 1;
                ps2_log("ipu: csc in  y[0..7]=%d %d %d %d %d %d %d %d  "
                        "cb[0..3]=%d %d %d %d  cr[0..3]=%d %d %d %d",
                        y[0], y[1], y[2], y[3], y[4], y[5], y[6], y[7],
                        cb[0], cb[1], cb[2], cb[3], cr[0], cr[1], cr[2], cr[3]);
            }
        }
        emit_rgb(y, cb, cr, ofm, dte, 0);
    }
    in_compact();
}

static void cmd_pack(u32 opt) {
    u32 mbc = opt & 0x7FFu;
    int dte = (int)((opt >> 26) & 1u);
    int ofm = (int)((opt >> 27) & 1u);
    u32 m;
    if (bit_avail() < mbc * (64u * 128u)) {
        ipu_starved = 1;
        ipu_need_qw = (mbc * (64u * 128u) - bit_avail() + 127u) / 128u;
        return;
    }
    out_reset();
    for (m = 0; m < mbc; m++) {
        u32 px[256];
        int i;
        for (i = 0; i < 256; i++) {
            u32 a = bit_get(8), b = bit_get(8), c = bit_get(8), d = bit_get(8);
            px[i] = a | (b << 8) | (c << 16) | (d << 24);
        }
        if (ofm) {
            u16 o[256];
            for (i = 0; i < 256; i++)
                o[i] = rgb32_to_rgb16(px[i], i & 15, i >> 4, dte);
            out_push(o, sizeof o);
        } else {
            u8 o[128];
            memset(o, 0, sizeof o);
            for (i = 0; i < 256; i++) {
                u32 idx = vq_index(rgb32_to_rgb16(px[i], i & 15, i >> 4, dte));
                if (i & 1) o[i >> 1] |= (u8)(idx << 4);
                else       o[i >> 1] |= (u8)idx;
            }
            out_push(o, sizeof o);
        }
    }
    in_compact();
}

static void ipu_run(u32 cmd) {
    u32 code = cmd >> 28, opt = cmd & 0x0FFFFFFFu;
    tables_init();
    stat_cmd[code]++;
    ipu_scd = ipu_ecd = 0;
    switch (code) {
    case IPU_BCLR:  cmd_bclr(opt); break;
    case IPU_IDEC:  cmd_idec(opt); break;
    case IPU_BDEC:  cmd_bdec(opt); break;
    case IPU_VDEC:  cmd_vdec((opt >> 26) & 3u, opt & 0x3Fu); break;
    case IPU_FDEC:  cmd_fdec(opt & 0x3Fu); break;
    case IPU_SETIQ: cmd_setiq(opt); break;
    case IPU_SETVQ: cmd_setvq(); break;
    case IPU_CSC:   cmd_csc(opt); break;
    case IPU_PACK:  cmd_pack(opt); break;
    case IPU_SETTH: cmd_setth(opt); break;
    default: break;
    }
}

u32 ps2_ipu_want(void) { return ipu_need_qw; }

u32 ps2_ipu_out_pos(void) { return out_pos; }

u32 ps2_ipu_out_avail(void) {
    return out_len > out_pos ? (out_len - out_pos) / 16u : 0u;
}

u32 ps2_ipu_in_space(void) {
    u32 have = in_len / 16u;
    u32 used = (u32)(in_bit >> 7);
    u32 held = have > used ? have - used : 0u;
    if (ipu_pending) return 8u;
    return held >= 8u ? 0u : 8u - held;
}

#define IPU_CMD_LOG 48
static struct { u32 cmd, in_len, chcr, madr, qwc; u64 in_bit; } cmd_log[IPU_CMD_LOG];
static u32 cmd_log_n;

static void cmd_log_push(u32 cmd) {
    u32 i = cmd_log_n++ % IPU_CMD_LOG, tadr;
    cmd_log[i].cmd = cmd;
    cmd_log[i].in_len = in_len;
    cmd_log[i].in_bit = in_bit;
    ps2_dmac_ipu_report(&cmd_log[i].chcr, &cmd_log[i].madr,
                        &cmd_log[i].qwc, &tadr);
}

static void cmd_log_report(void) {
    static const char *nm[10] = { "BCLR", "IDEC", "BDEC", "VDEC", "FDEC",
                                  "SETIQ", "SETVQ", "CSC", "PACK", "SETTH" };
    u32 have = cmd_log_n < IPU_CMD_LOG ? cmd_log_n : IPU_CMD_LOG;
    u32 first = cmd_log_n - have, k;
    if (!have) return;
    ps2_log("ipu: last %u commands, oldest first "
            "(toIPU state as each was issued):", have);
    for (k = 0; k < have; k++) {
        u32 i = (first + k) % IPU_CMD_LOG, code = cmd_log[i].cmd >> 28;
        ps2_log("   %-5s cmd=%08X  in %u bytes, cursor byte %u | "
                "toIPU chcr=%08X mod=%u madr=%08X qwc=%u",
                code < 10u ? nm[code] : "?", cmd_log[i].cmd,
                cmd_log[i].in_len, (u32)(cmd_log[i].in_bit >> 3),
                cmd_log[i].chcr, (cmd_log[i].chcr >> 2) & 3u,
                cmd_log[i].madr, cmd_log[i].qwc);
    }
}

static void ipu_command(u32 cmd) {
    cmd_log_push(cmd);
    ipu_pending = 0;
    ipu_in_command = 1;
    for (;;) {
        u32 before;
        snap_save(&cmd_snap);
        ipu_starved = 0;
        ipu_need_qw = 0;
        ipu_cmd_active = 1;
        ipu_run(cmd);
        ipu_cmd_active = 0;
        if (!ipu_starved) {
            if (ipu_ecd && !ipu_said_err) {
                u32 at = (u32)(in_bit >> 3), i, lo, hi;
                char hex[3 * 24 + 1];
                ipu_said_err = 1;
                lo = at > 8u ? at - 8u : 0u;
                hi = lo + 24u < in_len ? lo + 24u : in_len;
                for (i = lo; i < hi; i++)
                    snprintf(hex + (i - lo) * 3, 4, "%02X ", in_buf[i]);
                hex[(hi > lo ? (hi - lo) * 3 - 1 : 0)] = 0;
                ps2_log("ipu: cmd %08X decode error at bit %llu (byte %u of "
                        "%u); bytes %u..%u: %s", cmd,
                        (unsigned long long)in_bit, at, in_len, lo, hi, hex);
            }
            ipu_pending = 0; ipu_in_command = 0;
            ps2_dmac_ipu_drain();
            return;
        }
        snap_restore(&cmd_snap);
        ipu_pending = 1;
        before = in_len;
        ps2_dmac_ipu_pull();
        if (in_len == before) {
            if (!ipu_said_eos) {
                u32 chcr, madr, qwc, tadr;
                u32 i, lo, n = 0;
                char hex[3 * 32 + 1];
                ipu_said_eos = 1;
                lo = in_len > 32u ? in_len - 32u : 0u;
                for (i = lo; i < in_len && n < 32u; i++, n++)
                    snprintf(hex + n * 3, 4, "%02X ", in_buf[i]);
                hex[n ? n * 3 - 1 : 0] = 0;
                ps2_dmac_ipu_report(&chcr, &madr, &qwc, &tadr);
                ps2_log("ipu: dry -- cmd %08X, %u bytes in, cursor byte %u, "
                        "%u bits left, toIPU chcr=%08X qwc=%u; last bytes "
                        "%u..%u: %s", cmd, in_len, (u32)(in_bit >> 3),
                        bit_avail(), chcr, qwc, lo, in_len, hex);
            }
            break;
        }
    }
    ipu_in_command = 0;
    ipu_pending_cmd = cmd;
    ipu_pending = 1;
    stat_starve++;
    if (stat_starve == 1) {
        static const char *nm[10] = { "BCLR", "IDEC", "BDEC", "VDEC", "FDEC",
                                      "SETIQ", "SETVQ", "CSC", "PACK",
                                      "SETTH" };
        u32 code = cmd >> 28;
        u32 chcr, madr, qwc, tadr;
        ps2_dmac_ipu_report(&chcr, &madr, &qwc, &tadr);
        ps2_log("ipu: %s (cmd %08X) suspended -- %u bytes in, %llu bits "
                "consumed, %u bits left",
                code < 10u ? nm[code] : "?", cmd, in_len,
                (unsigned long long)in_bit, bit_avail());
        ps2_log("ipu: toIPU at that moment: chcr=%08X %s madr=%08X qwc=%u "
                "tadr=%08X -- %s", chcr,
                (chcr & 0x100u) ? "running" : "STOPPED", madr, qwc, tadr,
                (chcr & 0x100u) ? "channel had more to give"
                : qwc ? "STOPPED WITH DATA STILL QUEUED"
                      : "genuinely out of data");
        ps2_dmac_ipu_history();
        ps2_dump_trace("ipu-starve");
    }
}

void ps2_ipu_init(void) {
    int i;
    idct_init();
    tables_init();
    in_len = 0; in_bit = 0;
    out_reset();
    ipu_ctrl = 0; ipu_data = 0; ipu_cbp = 0;
    ipu_scd = ipu_ecd = 0;
    ipu_th0 = 0; ipu_th1 = 0;
    dc_pred[0] = dc_pred[1] = dc_pred[2] = 128;
    for (i = 0; i < 64; i++) { iq_intra_raster[i] = 16; iq_noni_raster[i] = 16; }
    memset(vq_clut, 0, sizeof vq_clut);
    memset(stat_cmd, 0, sizeof stat_cmd);
    stat_mb = stat_err = 0;
    stat_starve = stat_resume = 0;
    ipu_pending = 0; ipu_pending_cmd = 0;
    ipu_starved = 0; ipu_cmd_active = 0;
}

static u32 ipu_bp_reg(void) {
    u32 qw = (u32)(in_bit >> 7);
    u32 have = in_len / 16u;
    u32 held = have > qw ? have - qw : 0u;
    u32 ifc = held > 15u ? 15u : held;
    u32 fp  = held - ifc > 3u ? 3u : held - ifc;
    if (held > IPU_BP_MAX_QW && !ipu_said_bp) {
        ipu_said_bp = 1;
        ps2_log("ipu: IPU_BP cannot state a %u-quadword backlog (IFC+FP reach "
                "%u); a BCLR restart from here would be short by %u qw",
                held, IPU_BP_MAX_QW, held - IPU_BP_MAX_QW);
    }
    return (u32)(in_bit & 0x7Full) | (ifc << 8) | (fp << 16);
}

static u32 ipu_ctrl_reg(void) {
    u32 ifc = (ipu_bp_reg() >> 8) & 0xFu;
    u32 ofc = (out_len - out_pos) / 16u;
    if (ofc > 15u) ofc = 15u;
    return ifc | (ofc << 4) | ((ipu_cbp & 0x3Fu) << 8)
         | ((u32)ipu_ecd << 14) | ((u32)ipu_scd << 15)
         | (ipu_ctrl & IPU_CTRL_RW)
         | (ipu_pending ? 0x80000000u : 0u);
}

u32 ps2_ipu_read32(u32 a) {
    switch (a & 0x3Cu) {
    case 0x00: return ipu_data;
    case 0x04: return ipu_pending ? 0x80000000u : 0u;
    case 0x10: return ipu_ctrl_reg();
    case 0x14: return 0;
    case 0x20: return ipu_bp_reg();
    case 0x24: return 0;
    case 0x30: return bit_peek(32);
    case 0x34: return (ipu_pending || bit_avail() < 32u) ? 0x80000000u : 0u;
    default: return 0;
    }
}

void ps2_ipu_write32(u32 a, u32 v) {
    switch (a & 0x3Cu) {
    case 0x00: ipu_command(v); return;
    case 0x10:
        if (v & (1u << 30)) {
            in_len = 0; in_bit = 0;
            out_reset();
            ipu_scd = ipu_ecd = 0;
            ipu_cbp = 0;
            ipu_pending = 0;
            return;
        }
        ipu_ctrl = (ipu_ctrl & ~IPU_CTRL_RW) | (v & IPU_CTRL_RW);
        return;
    default: return;
    }
}

static int sc_on = -1;
static u32 sc_carry, sc_want;
static u64 sc_pos, sc_slices, sc_pics, sc_lines;

static void sc_scan(const u8 *b, u32 n) {
    u32 i;
    if (sc_on < 0) sc_on = PS2_ENV("PS2_TRACE_MPEG") ? 1 : 0;
    if (!sc_on) { sc_pos += n; return; }
    for (i = 0; i < n; i++) {
        u32 byte = b[i];
        if (sc_want) {
            const char *nm;
            sc_want = 0;
            if (byte && byte <= 0xAFu) { sc_slices++; goto shift; }
            switch (byte) {
            case 0x00: nm = "picture";         sc_pics++; break;
            case 0xB2: nm = "user_data";                  break;
            case 0xB3: nm = "SEQUENCE_HEADER";            break;
            case 0xB5: nm = "extension";                  break;
            case 0xB7: nm = "SEQUENCE_END";               break;
            case 0xB8: nm = "GOP";                        break;
            default:   nm = "reserved";                   break;
            }
            if (sc_lines < 4000u) {
                sc_lines++;
                ps2_log("mpeg: @%llu  00 00 01 %02X  %s%s",
                        (unsigned long long)(sc_pos + i - 3u), byte, nm,
                        byte == 0xB7u ? "   <<<< the decoder's exit" : "");
            }
        }
    shift:
        sc_carry = ((sc_carry << 8) | byte) & 0xFFFFFFu;
        if (sc_carry == 0x000001u) sc_want = 1;
    }
    sc_pos += n;
}

void ps2_ipu_dma_in(const void *qw, u32 n) {
    in_reserve(n * 16u);
    if (!in_buf) return;
    ipu_in_bytes += (u64)n * 16u;
    memcpy(in_buf + in_len, qw, n * 16u);
    sc_scan((const u8 *)qw, n * 16u);
    if (ipu_in_bytes <= (u64)n * 16u) {
        const u8 *b = (const u8 *)qw;
        u32 i, k = n * 16u < 32u ? n * 16u : 32u;
        char hex[3 * 32 + 1];
        for (i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02X ", b[i]);
        ps2_log("ipu: first bytes in: %s", hex);
    }
    in_len += n * 16u;
    if (ipu_pending && !ipu_in_command) {
        stat_resume++;
        ipu_command(ipu_pending_cmd);
    }
}

void ps2_ipu_dma_out(void *dst, u32 n) {
    u32 want = n * 16u;
    u32 have = out_len > out_pos ? out_len - out_pos : 0u;
    u32 take = want < have ? want : have;
    memcpy(dst, out_buf + out_pos, take);
    if (take < want) memset((u8 *)dst + take, 0, want - take);
    out_pos += take;
}

void ps2_ipu_state_save(ps2_state_put put, void *ud) {
    struct {
        u64 in_bit;
        u32 in_len, out_len, out_pos;
        u32 ctrl, data, cbp, th0, th1, need_qw, pending_cmd;
        int scd, ecd, pending;
    } s;
    memset(&s, 0, sizeof s);
    s.in_bit = in_bit;
    s.in_len = in_len;
    s.out_len = out_len;
    s.out_pos = out_pos;
    s.ctrl = ipu_ctrl;
    s.data = ipu_data;
    s.cbp = ipu_cbp;
    s.th0 = ipu_th0;
    s.th1 = ipu_th1;
    s.need_qw = ipu_need_qw;
    s.pending_cmd = ipu_pending_cmd;
    s.scd = ipu_scd;
    s.ecd = ipu_ecd;
    s.pending = ipu_pending;
    put(ud, "ipu", &s, sizeof s);
    put(ud, "ipu_in", in_buf, in_buf ? in_len : 0u);
    put(ud, "ipu_out", out_buf, out_buf ? out_len : 0u);
    put(ud, "ipu_iqi", iq_intra_raster, sizeof iq_intra_raster);
    put(ud, "ipu_iqn", iq_noni_raster, sizeof iq_noni_raster);
}

void ps2_ipu_report(void) {
    static const char *nm[10] = { "BCLR", "IDEC", "BDEC", "VDEC", "FDEC",
                                  "SETIQ", "SETVQ", "CSC", "PACK", "SETTH" };
    char line[256];
    int n = 0, i;
    u64 total = 0;
    for (i = 0; i < 10; i++) total += stat_cmd[i];
    if (!total) { ps2_log("ipu: idle, in %llu bytes", (unsigned long long)ipu_in_bytes); return; }
    for (i = 0; i < 10; i++)
        if (stat_cmd[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s:%llu",
                          nm[i], (unsigned long long)stat_cmd[i]);
    ps2_log("ipu:%s | in %llu bytes, %llu macroblocks, %llu decode errors,"
            " %llu stalls, %llu resumes%s",
            line, (unsigned long long)ipu_in_bytes,
            (unsigned long long)stat_mb, (unsigned long long)stat_err,
            (unsigned long long)stat_starve, (unsigned long long)stat_resume,
            ipu_pending ? ", SUSPENDED" : "");
    cmd_log_report();
    ps2_dmac_ipu_history();
    if (sc_on > 0) {
        ps2_log("mpeg: stream fed %llu bytes: %llu pictures, %llu slices, "
                "%llu start codes named", (unsigned long long)sc_pos,
                (unsigned long long)sc_pics, (unsigned long long)sc_slices,
                (unsigned long long)sc_lines);
        ps2_dmac_ipu_out_history();
        ps2_gs_trx_history();
    }
}

int ps2_ipu_selftest(const char *path, int max_frames) {
    FILE *fp = fopen(path, "rb");
    u8 *data;
    long size;
    u32 w, h, nframes, mbx, mby, f;
    u32 *frame;
    int bad = 0;
    if (!fp) { ps2_log("ipu-test: cannot open %s", path); return 1; }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = (u8 *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, fp) != (size_t)size) {
        ps2_log("ipu-test: cannot read %s", path);
        fclose(fp);
        free(data);
        return 1;
    }
    fclose(fp);
    if (memcmp(data, "ipum", 4)) {
        ps2_log("ipu-test: %s is not an .ipu stream", path);
        free(data);
        return 1;
    }
    w = (u32)data[8] | ((u32)data[9] << 8);
    h = (u32)data[10] | ((u32)data[11] << 8);
    nframes = (u32)data[12] | ((u32)data[13] << 8)
            | ((u32)data[14] << 16) | ((u32)data[15] << 24);
    mbx = w / 16u; mby = h / 16u;
    ps2_log("ipu-test: %s  %ux%u, %u frames, %ux%u macroblocks",
            path, w, h, nframes, mbx, mby);

    ps2_ipu_init();
    ps2_ipu_write32(0x10002010u, 1u << 30);
    ps2_ipu_write32(0x10002000u, 0u);
    ps2_ipu_dma_in(data + 16, (u32)((size - 16) / 16));

    frame = (u32 *)malloc((size_t)mbx * mby * 256u * 4u);
    if (!frame) { free(data); return 1; }

    if (max_frames <= 0 || (u32)max_frames > nframes) max_frames = (int)nframes;
    for (f = 0; f < (u32)max_frames; f++) {
        u32 flags, mb;
        char name[64];
        FILE *out;
        ps2_ipu_write32(0x10002000u, 0x40000000u);
        flags = (ps2_ipu_read32(0x10002000u) >> 24) & 0xFFu;
        ps2_ipu_write32(0x10002000u, 0x40000008u);
        ps2_ipu_write32(0x10002010u, (flags & ~0x04u) << 16);
        ps2_ipu_write32(0x10002000u,
                        0x10000000u | (((flags >> 2) & 1u) << 24) | (1u << 16));
        ps2_ipu_dma_out(frame, mbx * mby * 64u);
        ps2_ipu_write32(0x10002000u, 0x40000020u);

        if (f >= 3u && f != (u32)max_frames - 1u) continue;
        snprintf(name, sizeof name, "ipu_test_%03u.ppm", f);
        out = fopen(name, "wb");
        if (!out) continue;
        fprintf(out, "P6\n%u %u\n255\n", w, h);
        for (mb = 0; mb < h; mb++) {
            u32 x;
            for (x = 0; x < w; x++) {
                u32 p = frame[((mb / 16u) * mbx + (x / 16u)) * 256u
                            + (mb % 16u) * 16u + (x % 16u)];
                fputc((int)(p & 0xFFu), out);
                fputc((int)((p >> 8) & 0xFFu), out);
                fputc((int)((p >> 16) & 0xFFu), out);
            }
        }
        fclose(out);
        ps2_log("ipu-test: frame %u -> %s", f, name);
    }
    ps2_ipu_report();
    free(frame);
    free(data);
    return bad;
}
