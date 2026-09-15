#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_capture.h"
#include <stdlib.h>

void ps2_gif_transfer(const ps2_reg128 *data, u32 qwc);
u32  ps2_gif_transfer_eop(const ps2_reg128 *data, u32 max);
u32  ps2_gif_kick(const u8 *vumem, u32 mem_size, u32 addr_qw);
extern int ps2_gif_path;
#include <stdio.h>
#include <math.h>
#include <string.h>

u8 *ps2_vu1_memory(void);
u8 *ps2_vu1_microcode(void);
void ps2_gs_write_reg(u32 reg, u64 val);
void ps2_intc_raise(int irq);
#define PS2_INTC_VIF0 4
#define PS2_INTC_VIF1 5

typedef struct {
    u32 code;
    u32 num, cmd, imm;
    u32 cycle_cl, cycle_wl;
    u32 offset, base, itop, itops, top, tops;
    u32 dbf;
    u32 mode, mask;
    u32 row[4], col[4];
    u32 wait;
    u32 addr;
    u32 unpack_vn, unpack_vl, unpack_m, unpack_usn;
    u64 bits;
    u32 nbits;
    u32 elem[4];
    u32 elem_n;
    u32 unpack_num;
    u32 unpack_written;
    u32 unpack_base;
    u32 unpack_cl, unpack_wl;
    u32 mpg_addr;
    u32 acc[4];
    u32 accn;
    u32 direct_left;
    u32 mark, err, stat;
    u32 err_stall;
    int is_vif1;
    u32 word_lane, unpack_align, unpack_index;
} ps2_vif;

static ps2_vif vif[2];
static ps2_vu vu1;
static int vu1_ready;

void ps2_vu_control_stop(ps2_ctx *ctx, u32 val) {
    void ps2_gfxq_drain(void);
    if (!(val & 0x303u)) return;
    ps2_gfxq_drain();
    if (val & 3u) { ctx->vu0.running = 0; vif[0].stat &= ~4u; }
    if (val & 0x300u) { vu1.running = 0; vif[1].stat &= ~4u; }
}

static u64 vu_stat_mpg, vu_stat_mscal, vu_stat_xgkick, vu_stat_unpack;
static u64 vu_stat_xgkick_qw;
static u64 vu_stat_insns, vu_stat_guard_hits;
static u64 speed_vu1_runs, speed_vu1_insns, speed_vu1_guards;
void ps2_vu1_speed_counters(u64 *runs, u64 *insns, u64 *guards) {
    *runs=speed_vu1_runs; *insns=speed_vu1_insns; *guards=speed_vu1_guards;
}
#define VU_RUN_LIMIT_DEFAULT 1048576u
static u32 vu_run_limit = VU_RUN_LIMIT_DEFAULT;
static u32 vu0_run_limit = 20000u;
static u64 vu_mpg_at[64];
static u64 vu_mscal_at[64];
static u64 vu_mscnt_n;
static u32 vu_mpg_lo = 0xFFFFFFFFu, vu_mpg_hi;

#define VU_PROG_N 256
typedef struct {
    u64 crc;
    u32 lo, hi;
    u64 activations;
    u64 insns;
    u64 kicks;
    u32 entries[32];
    u64 entry_n[32];
    u32 n_entries;
    u8 *image;
    u32 image_size;
} vu_prog;
static vu_prog vu_progs[VU_PROG_N];
static u32 vu_prog_n;
static u64 vu_prog_dropped;
static int vu_census_on = -1;
static int vu_prog_dirty = 1;
static u64 vu_prog_crc;

static int vu_census_enabled(void) {
    if (vu_census_on < 0) {
        const char *e = getenv("PS2_VU_CENSUS");
        vu_census_on = (e && *e && *e != '0') ? 1 : 0;
        if (vu_census_on)
            ps2_log("vu1: microprogram census armed (PS2_VU_CENSUS) -- "
                    "distinct images and entry points will be written to "
                    "out/vu_programs/");
    }
    return vu_census_on;
}

static u64 vu_hash(const u8 *p, u32 n) {
    u64 h = 1469598103934665603ull;
    u32 i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static u64 vu_top_hist[66];
static u64 vu_runlen_hist[24];
static u64 vu_runs_with_kick;
static u64 vu_low_hist[128];

#define VU_PROF_SLOTS (PS2_VU1_MICRO_SIZE / 8u)
static u64 vu_pc_hist[VU_PROF_SLOTS];
static u64 vu_prog_insns[VU_PROF_SLOTS];
static u64 vu_prog_runs[VU_PROF_SLOTS];
static u64 vu_up_hist[256];
static u64 vu_lo_sub_hist[2048];
static int vu_profile = -1;

static int vu_profile_on(void) {
    if (vu_profile < 0) vu_profile = getenv("PS2_VU_PROFILE") ? 1 : 0;
    return vu_profile;
}
#define VU_BR_N 256
typedef struct { u32 pc; u64 taken, not_taken; } vu_branch_rec;
static vu_branch_rec vu_br[VU_BR_N];
static unsigned vu_br_n;

static void vu_branch_seen(u32 pc, int taken) {
    unsigned i;
    for (i = 0; i < vu_br_n; i++)
        if (vu_br[i].pc == pc) { if (taken) vu_br[i].taken++; else vu_br[i].not_taken++; return; }
    if (vu_br_n >= VU_BR_N) return;
    i = vu_br_n++;
    vu_br[i].pc = pc;
    vu_br[i].taken = taken ? 1u : 0u;
    vu_br[i].not_taken = taken ? 0u : 1u;
}

void ps2_vu_branch_report(void) {
    unsigned i, j;
    if (!vu_br_n) return;
    ps2_log("VU1 branch outcomes (%u distinct), by instruction:", vu_br_n);
    for (i = 0; i < vu_br_n; i++) {
        unsigned best = i;
        for (j = i + 1; j < vu_br_n; j++)
            if (vu_br[j].pc < vu_br[best].pc) best = j;
        if (best != i) {
            vu_branch_rec t = vu_br[i];
            vu_br[i] = vu_br[best]; vu_br[best] = t;
        }
        ps2_log("   instr %-5u  taken %-10llu  fell through %-10llu",
                vu_br[i].pc / 8u, (unsigned long long)vu_br[i].taken,
                (unsigned long long)vu_br[i].not_taken);
    }
}
static u64 vu_sp_lo6[64];
static u64 vu_sp_hi5[32];
static u64 vu_sp_pair[64][32];
static u64 vif_cmd_hist[2][256];
static u32 vu_unknown_lower[64];
static u32 vu_unknown_n;

#define VIF_RING_N 160
enum { VIF_EV_CODE = 0, VIF_EV_TAG };
typedef struct { u8 kind, ch; u32 a, b, c; } vif_ring_ent;
static vif_ring_ent vif_ring[VIF_RING_N];
static u64 vif_ring_seq;
static u64 vif_bad_cmd, vif_bad_mscal, vif_irq_codes, vif_desync_reports;
static u64 vif_stalls;

static void vif_ring_push(u8 kind, int ch, u32 a, u32 b, u32 c) {
    vif_ring_ent *e = &vif_ring[vif_ring_seq++ % VIF_RING_N];
    e->kind = kind; e->ch = (u8)ch; e->a = a; e->b = b; e->c = c;
}

static void vif_ring_setwait(u32 words) {
    if (!vif_ring_seq) return;
    vif_ring[(vif_ring_seq - 1u) % VIF_RING_N].b = words;
}

void ps2_vif_note_tag(int ch, u32 tadr, u32 tag_lo, u32 tag_hi) {
    if (ch != 0 && ch != 1) return;
    vif_ring_push(VIF_EV_TAG, ch, tadr, tag_lo, tag_hi);
}

static const char *vif_cmd_name(u32 cmd) {
    switch (cmd) {
    case 0x00: return "NOP";      case 0x01: return "STCYCL";
    case 0x02: return "OFFSET";   case 0x03: return "BASE";
    case 0x04: return "ITOP";     case 0x05: return "STMOD";
    case 0x06: return "MSKPATH3"; case 0x07: return "MARK";
    case 0x10: return "FLUSHE";   case 0x11: return "FLUSH";
    case 0x13: return "FLUSHA";   case 0x14: return "MSCAL";
    case 0x15: return "MSCALF";   case 0x17: return "MSCNT";
    case 0x20: return "STMASK";   case 0x30: return "STROW";
    case 0x31: return "STCOL";    case 0x4A: return "MPG";
    case 0x50: return "DIRECT";   case 0x51: return "DIRECTHL";
    default:   return cmd >= 0x60u ? "UNPACK" : "?INVALID";
    }
}

static int vif_cmd_valid(u32 cmd) {
    switch (cmd) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: case 0x14:
    case 0x15: case 0x17: case 0x20: case 0x30: case 0x31: case 0x4A:
    case 0x50: case 0x51:
        return 1;
    default:
        if (cmd < 0x60u) return 0;
        return (cmd & 3u) != 3u || (cmd & 0xCu) == 0xCu;
    }
}

static void vif_desync_report(const char *why, int ch, u32 code) {
    u32 n = vif_ring_seq < (u64)VIF_RING_N ? (u32)vif_ring_seq : VIF_RING_N;
    u32 i;
    if (vif_desync_reports++ >= 6u) return;
    ps2_log("==== VIF%d STREAM DESYNC (%llu): %s -- code %08X ====", ch,
            (unsigned long long)vif_desync_reports, why, code);
    ps2_log("   last %u events, oldest first:", n);
    for (i = 0; i < n; i++) {
        const vif_ring_ent *e =
            &vif_ring[(u32)((vif_ring_seq - n + i) % (u64)VIF_RING_N)];
        if (e->kind == VIF_EV_TAG) {
            u32 qwc = e->b & 0xFFFFu, id = (e->b >> 28) & 7u;
            static const char *idn[8] = { "refe", "cnt", "next", "ref",
                                          "refs", "call", "ret", "end" };
            ps2_log("   -- ch%u DMAtag @%08X  %s qwc=%u addr=%08X",
                    e->ch, e->a, idn[id], qwc, e->c & 0xFFFFFFF0u);
        } else {
            u32 c = (e->a >> 24) & 0x7Fu;
            ps2_log("      ch%u code %08X  %-8s num=%-3u imm=%04X  "
                    "cl=%u wl=%u -> %u payload words%s",
                    e->ch, e->a, vif_cmd_name(c), (e->a >> 16) & 0xFFu,
                    e->a & 0xFFFFu, (e->c >> 8) & 0xFFu, e->c & 0xFFu, e->b,
                    (e->a >> 31) ? "  [i]" : "");
        }
    }
    ps2_log("==== end of VIF%d desync report ====", ch);
}

void ps2_vif_sync_report(void) {
    if (!vif_bad_cmd && !vif_bad_mscal && !vif_irq_codes) return;
    ps2_log("VIF stream: %llu undefined VIFcodes, %llu MSCALs rejected as "
            "out of micro memory, %llu codes carried the interrupt bit",
            (unsigned long long)vif_bad_cmd,
            (unsigned long long)vif_bad_mscal,
            (unsigned long long)vif_irq_codes);
    if (vif_stalls)
        ps2_log("VIF stream: %llu transfer(s) abandoned on VIF_STAT.ER1 "
                "(the rest of each was discarded, not executed)",
                (unsigned long long)vif_stalls);
}

void ps2_vu1_init(void) {
    { const char *e = getenv("PS2_VU_LIMIT");
      if (e) { unsigned long v = strtoul(e, NULL, 0);
               if (v >= 1000ul) vu_run_limit = vu0_run_limit = (u32)v; } }
    vu1.mem = ps2_vu1_memory();
    vu1.micro = ps2_vu1_microcode();
    vu1.mem_size = PS2_VU1_MEM_SIZE;
    vu1.micro_size = PS2_VU1_MICRO_SIZE;
    vu1.vf[0].f[0] = 0.0f;
    vu1.vf[0].f[1] = 0.0f;
    vu1.vf[0].f[2] = 0.0f;
    vu1.vf[0].f[3] = 1.0f;
    vif[1].is_vif1 = 1;
    vu1_ready = 1;
}

static float ps2_sqrtf(float a)  { return sqrtf(a); }
static float ps2_sinf(float a)   { return sinf(a); }
static float ps2_atanf(float a)  { return atanf(a); }
static float ps2_atan2f(float y, float x) { return atan2f(y, x); }
static float ps2_expf(float a)   { return expf(a); }

#if !defined(PS2_VU_SCALAR) \
    && (defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64))
#  include <emmintrin.h>
#  define VU_SIMD 1
typedef __m128 vu_f4;
#else
#  define VU_SIMD 0
typedef struct { float f[4]; } vu_f4;
#endif

static const u32 vu_dest_mask[16][4] __attribute__((aligned(16))) = {
    {0,0,0,0},                   {0,0,0,~0u},
    {0,0,~0u,0},                 {0,0,~0u,~0u},
    {0,~0u,0,0},                 {0,~0u,0,~0u},
    {0,~0u,~0u,0},               {0,~0u,~0u,~0u},
    {~0u,0,0,0},                 {~0u,0,0,~0u},
    {~0u,0,~0u,0},               {~0u,0,~0u,~0u},
    {~0u,~0u,0,0},               {~0u,~0u,0,~0u},
    {~0u,~0u,~0u,0},             {~0u,~0u,~0u,~0u}
};

static const u8 vu_rev4[16] = {
    0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
};

#if VU_SIMD

#define VU_EXP  0x7F800000u
#define VU_SIGN 0x80000000u
#define VU_ABS  0x7FFFFFFFu
#define VU_FMAX 0x7F7FFFFFu

PS2_INLINE __m128 vu_ftz4(__m128 x) {
    __m128i xi = _mm_castps_si128(x);
    __m128i den = _mm_cmpeq_epi32(_mm_and_si128(xi, _mm_set1_epi32((int)VU_EXP)),
                                  _mm_setzero_si128());
    return _mm_castsi128_ps(
        _mm_andnot_si128(_mm_and_si128(den, _mm_set1_epi32((int)VU_ABS)), xi));
}

PS2_INLINE __m128 vu_clamp4(__m128 r) {
    __m128i ri = _mm_castps_si128(r);
    __m128i ab = _mm_and_si128(ri, _mm_set1_epi32((int)VU_ABS));
    __m128i big = _mm_cmpgt_epi32(ab, _mm_set1_epi32((int)VU_FMAX));
    __m128i nan = _mm_cmpgt_epi32(ab, _mm_set1_epi32((int)VU_EXP));
    __m128i sgn = _mm_and_si128(ri, _mm_set1_epi32((int)VU_SIGN));
    __m128i sat = _mm_or_si128(_mm_andnot_si128(nan, sgn),
                               _mm_set1_epi32((int)VU_FMAX));
    return _mm_castsi128_ps(
        _mm_or_si128(_mm_and_si128(big, sat), _mm_andnot_si128(big, ri)));
}

PS2_INLINE vu_f4 vu_ld(const ps2_vf *v) {
    return _mm_load_ps(v->f);
}
PS2_INLINE vu_f4 vu_set1(float f)      { return _mm_set1_ps(f); }
PS2_INLINE vu_f4 vu_add4(vu_f4 a, vu_f4 b) {
    return vu_clamp4(_mm_add_ps(vu_ftz4(a), vu_ftz4(b)));
}
PS2_INLINE vu_f4 vu_sub4(vu_f4 a, vu_f4 b) {
    return vu_clamp4(_mm_sub_ps(vu_ftz4(a), vu_ftz4(b)));
}
PS2_INLINE vu_f4 vu_mul4(vu_f4 a, vu_f4 b) {
    return vu_clamp4(_mm_mul_ps(vu_ftz4(a), vu_ftz4(b)));
}
PS2_INLINE vu_f4 vu_select4(vu_f4 a, vu_f4 b, int minimum) {
    __m128i x = _mm_castps_si128(a), y = _mm_castps_si128(b);
    __m128i mask = _mm_set1_epi32(0x7FFFFFFF);
    __m128i kx = _mm_xor_si128(x, _mm_and_si128(_mm_srai_epi32(x, 31), mask));
    __m128i ky = _mm_xor_si128(y, _mm_and_si128(_mm_srai_epi32(y, 31), mask));
    __m128i take = minimum ? _mm_cmpgt_epi32(ky, kx) : _mm_cmpgt_epi32(kx, ky);
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(take, x), _mm_andnot_si128(take, y)));
}
PS2_INLINE vu_f4 vu_max4(vu_f4 a, vu_f4 b) { return vu_select4(a, b, 0); }
PS2_INLINE vu_f4 vu_min4(vu_f4 a, vu_f4 b) { return vu_select4(a, b, 1); }

PS2_INLINE vu_f4 vu_madd4(vu_f4 acc, vu_f4 a, vu_f4 b) {
    return vu_clamp4(_mm_add_ps(acc, vu_mul4(a, b)));
}
PS2_INLINE vu_f4 vu_msub4(vu_f4 acc, vu_f4 a, vu_f4 b) {
    return vu_clamp4(_mm_sub_ps(acc, vu_mul4(a, b)));
}

PS2_INLINE vu_f4 vu_itof4(const ps2_vf *s, int sh) {
    return _mm_mul_ps(_mm_cvtepi32_ps(_mm_load_si128((const __m128i *)s->u)),
                      _mm_set1_ps(1.0f / (float)(1u << sh)));
}

PS2_INLINE vu_f4 vu_ftoi4(const ps2_vf *s, int sh) {
    __m128 x = _mm_mul_ps(_mm_load_ps(s->f), _mm_set1_ps((float)(1u << sh)));
    __m128i t = _mm_cvttps_epi32(x);
    __m128i bad = _mm_cmpeq_epi32(t, _mm_set1_epi32((int)0x80000000));
    __m128i neg = _mm_srai_epi32(_mm_castps_si128(x), 31);
    __m128i sat = _mm_or_si128(_mm_and_si128(neg, _mm_set1_epi32((int)0x80000000)),
                               _mm_andnot_si128(neg, _mm_set1_epi32(0x7FFFFFFF)));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bad, sat),
                                         _mm_andnot_si128(bad, t)));
}

PS2_INLINE vu_f4 vu_abs4(vu_f4 a) {
    return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32((int)VU_ABS)));
}
PS2_INLINE vu_f4 vu_perm_yzx(vu_f4 v) {
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1));
}
PS2_INLINE vu_f4 vu_perm_zxy(vu_f4 v) {
    return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2));
}

PS2_INLINE void vu_blend(ps2_vf *d, int dest, vu_f4 v) {
    if (dest == 0xF) { _mm_store_ps(d->f, v); return; }
    {
        __m128 m = _mm_load_ps((const float *)vu_dest_mask[dest]);
        _mm_store_ps(d->f, _mm_or_ps(_mm_and_ps(m, v),
                                     _mm_andnot_ps(m, _mm_load_ps(d->f))));
    }
}

PS2_INLINE u32 vu_mac4(vu_f4 v, u32 dest) {
    __m128i vi = _mm_castps_si128(v);
    u32 z = vu_rev4[_mm_movemask_ps(_mm_castsi128_ps(
                _mm_cmpeq_epi32(_mm_and_si128(vi, _mm_set1_epi32((int)VU_ABS)),
                                _mm_setzero_si128())))];
    u32 s = vu_rev4[_mm_movemask_ps(v)] & ~z;
    return (z & dest) | ((s & dest) << 4);
}

PS2_INLINE float vu_lane(vu_f4 v, int i) {
    ps2_vf t;
    _mm_store_ps(t.f, v);
    return t.f[i];
}

#else

PS2_INLINE vu_f4 vu_ld(const ps2_vf *v) {
    vu_f4 r; int i; for (i = 0; i < 4; i++) r.f[i] = v->f[i]; return r;
}
PS2_INLINE vu_f4 vu_set1(float f) {
    vu_f4 r; int i; for (i = 0; i < 4; i++) r.f[i] = f; return r;
}
#define VU_BIN(name, expr) \
    PS2_INLINE vu_f4 name(vu_f4 a, vu_f4 b) { \
        vu_f4 r; int i; for (i = 0; i < 4; i++) { \
            float x = a.f[i], y = b.f[i]; r.f[i] = (expr); } return r; }
VU_BIN(vu_add4, ps2_fadd(x, y))
VU_BIN(vu_sub4, ps2_fsub(x, y))
VU_BIN(vu_mul4, ps2_fmul(x, y))
VU_BIN(vu_max4, ps2_vu_select(x, y, 0))
VU_BIN(vu_min4, ps2_vu_select(x, y, 1))
#undef VU_BIN
PS2_INLINE vu_f4 vu_madd4(vu_f4 acc, vu_f4 a, vu_f4 b) {
    return vu_add4(acc, vu_mul4(a, b));
}
PS2_INLINE vu_f4 vu_msub4(vu_f4 acc, vu_f4 a, vu_f4 b) {
    return vu_sub4(acc, vu_mul4(a, b));
}
PS2_INLINE vu_f4 vu_itof4(const ps2_vf *s, int sh) {
    vu_f4 r; int i;
    float sc = 1.0f / (float)(1u << sh);
    for (i = 0; i < 4; i++) r.f[i] = (float)s->s[i] * sc;
    return r;
}
PS2_INLINE vu_f4 vu_ftoi4(const ps2_vf *s, int sh) {
    vu_f4 r; int i;
    float sc = (float)(1u << sh);
    for (i = 0; i < 4; i++) {
        s32 v = ps2_cvt_w_s(s->f[i] * sc);
        memcpy(&r.f[i], &v, 4);
    }
    return r;
}
PS2_INLINE vu_f4 vu_abs4(vu_f4 a) {
    vu_f4 r; int i;
    for (i = 0; i < 4; i++) { u32 u; memcpy(&u, &a.f[i], 4); u &= 0x7FFFFFFFu;
                              memcpy(&r.f[i], &u, 4); }
    return r;
}
PS2_INLINE void vu_blend(ps2_vf *d, int dest, vu_f4 v) {
    int i;
    for (i = 0; i < 4; i++) if ((dest >> (3 - i)) & 1) d->f[i] = v.f[i];
}
PS2_INLINE u32 vu_mac4(vu_f4 v, u32 dest) {
    u32 mac = 0; int i;
    for (i = 0; i < 4; i++) {
        u32 u, bit = (u32)(3 - i);
        if (!((dest >> (3 - i)) & 1)) continue;
        memcpy(&u, &v.f[i], 4);
        if ((u & 0x7FFFFFFFu) == 0u) mac |= 1u << bit;
        else if (u & 0x80000000u)    mac |= 1u << (4u + bit);
    }
    return mac;
}
PS2_INLINE float vu_lane(vu_f4 v, int i) { return v.f[i]; }
PS2_INLINE vu_f4 vu_perm_yzx(vu_f4 v) {
    vu_f4 r; r.f[0] = v.f[1]; r.f[1] = v.f[2]; r.f[2] = v.f[0]; r.f[3] = v.f[3];
    return r;
}
PS2_INLINE vu_f4 vu_perm_zxy(vu_f4 v) {
    vu_f4 r; r.f[0] = v.f[2]; r.f[1] = v.f[0]; r.f[2] = v.f[1]; r.f[3] = v.f[3];
    return r;
}

#endif

PS2_INLINE void vu_wr(ps2_vu *vu, int reg, int dest, vu_f4 v) {
    if (reg == 0) return;
    vu_blend(&vu->vf[reg], dest, v);
}

static void vu_store(ps2_vu *vu, int reg, int dest, const float v[4]) {
    ps2_vf *d;
    if (reg == 0) return;
    d = &vu->vf[reg];
    if (dest & 8) d->f[0] = v[0];
    if (dest & 4) d->f[1] = v[1];
    if (dest & 2) d->f[2] = v[2];
    if (dest & 1) d->f[3] = v[3];
}

static void vu_set_flags(ps2_vu *vu, u32 dest, vu_f4 r, u32 fslot) {
    u32 mac = vu_mac4(r, dest);
    {
        u32 st = 0, ns;
        if (mac & 0x000Fu) st |= 1u << 0;
        if (mac & 0x00F0u) st |= 1u << 1;
        if (mac & 0x0F00u) st |= 1u << 2;
        if (mac & 0xF000u) st |= 1u << 3;
        ns = vu->fpipe[fslot].valid ? vu->fpipe[fslot].status
                                        : vu->status;
        ns = (ns & 0xFF0u) | st | ((st & 0xFu) << 6);
        vu->fpipe[fslot].mac = mac;
        vu->fpipe[fslot].status = ns;
        vu->fpipe[fslot].valid = 1;
    }
}

static void vu_upper(ps2_vu *vu, u32 ins, u32 fslot) {
    u32 funct = ins & 0x3Fu;
    u32 dest = (ins >> 21) & 0xFu;
    int ft = (int)((ins >> 16) & 0x1Fu);
    int fs = (int)((ins >> 11) & 0x1Fu);
    int fd = (int)((ins >> 6) & 0x1Fu);
    int bc = (int)(ins & 3u);
    vu_f4 r;
    int set_flags = 1;

    if ((funct & 0x3Cu) == 0x3Cu) {
        u32 sub = (ins & 3u) | ((ins >> 4) & 0x7Cu);
        switch (sub) {
        case 0x00: case 0x01: case 0x02: case 0x03:
            r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
            goto store_acc;
        case 0x04: case 0x05: case 0x06: case 0x07:
            r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
            goto store_acc;
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]),
                         vu_set1(vu->vf[ft].f[bc]));
            goto store_acc;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]),
                         vu_set1(vu->vf[ft].f[bc]));
            goto store_acc;
        case 0x10: case 0x11: case 0x12: case 0x13: {
            static const int sh[4] = {0, 4, 12, 15};
            vu_wr(vu, ft, (int)dest, vu_itof4(&vu->vf[fs], sh[sub & 3]));
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {
            static const int sh[4] = {0, 4, 12, 15};
            vu_wr(vu, ft, (int)dest, vu_ftoi4(&vu->vf[fs], sh[sub & 3]));
            return;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B:
            r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
            goto store_acc;
        case 0x1C:
            r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->q));
            goto store_acc;
        case 0x1D:
            vu_wr(vu, ft, (int)dest, vu_abs4(vu_ld(&vu->vf[fs])));
            return;
        case 0x1E:
            r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->i));
            goto store_acc;
        case 0x1F: {
            u32 c = (vu->clip << 6) & 0xFFFFFFu;
            float w = vu->vf[ft].f[3];
            float aw = w < 0.0f ? -w : w;
            int i;
            for (i = 0; i < 3; i++) {
                float v = vu->vf[fs].f[i];
                if (v > aw) c |= 1u << (i * 2);
                if (v < -aw) c |= 1u << (i * 2 + 1);
            }
            vu->clip = c & 0xFFFFFFu;
            return;
        }
        case 0x20: r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->q)); goto store_acc;
        case 0x21: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->q)); goto store_acc;
        case 0x22: r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); goto store_acc;
        case 0x23: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->i)); goto store_acc;
        case 0x24: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->q)); goto store_acc;
        case 0x25: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->q)); goto store_acc;
        case 0x26: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); goto store_acc;
        case 0x27: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->i)); goto store_acc;
        case 0x28: r = vu_add4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); goto store_acc;
        case 0x29: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); goto store_acc;
        case 0x2A: r = vu_mul4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); goto store_acc;
        case 0x2C: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); goto store_acc;
        case 0x2D: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); goto store_acc;
        case 0x2E:
            vu_blend(&vu->acc, 0xE,
                     vu_mul4(vu_perm_yzx(vu_ld(&vu->vf[fs])),
                             vu_perm_zxy(vu_ld(&vu->vf[ft]))));
            return;
        case 0x2F:
            return;
        default:
            return;
        }
    store_acc:
        vu_set_flags(vu, dest, r, fslot);
        vu_blend(&vu->acc, (int)dest, r);
        return;
    }

    switch (funct) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]),
                     vu_set1(vu->vf[ft].f[bc]));
        break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]),
                     vu_set1(vu->vf[ft].f[bc]));
        break;
    case 0x10: case 0x11: case 0x12: case 0x13:
        r = vu_max4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
        set_flags = 0;
        break;
    case 0x14: case 0x15: case 0x16: case 0x17:
        r = vu_min4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
        set_flags = 0;
        break;
    case 0x18: case 0x19: case 0x1A: case 0x1B:
        r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->vf[ft].f[bc]));
        break;
    case 0x1C: r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->q)); break;
    case 0x1D: r = vu_max4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); set_flags = 0; break;
    case 0x1E: r = vu_mul4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); break;
    case 0x1F: r = vu_min4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); set_flags = 0; break;
    case 0x20: r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->q)); break;
    case 0x21: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->q)); break;
    case 0x22: r = vu_add4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); break;
    case 0x23: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->i)); break;
    case 0x24: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->q)); break;
    case 0x25: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->q)); break;
    case 0x26: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_set1(vu->i)); break;
    case 0x27: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_set1(vu->i)); break;
    case 0x28: r = vu_add4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    case 0x29: r = vu_madd4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    case 0x2A: r = vu_mul4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    case 0x2B: r = vu_max4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); set_flags = 0; break;
    case 0x2C: r = vu_sub4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    case 0x2D: r = vu_msub4(vu_ld(&vu->acc), vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    case 0x2E:
        r = vu_msub4(vu_ld(&vu->acc), vu_perm_yzx(vu_ld(&vu->vf[fs])),
                     vu_perm_zxy(vu_ld(&vu->vf[ft])));
        vu_set_flags(vu, 0xE, r, fslot);
        vu_wr(vu, fd, 0xE, r);
        return;
    case 0x2F: r = vu_min4(vu_ld(&vu->vf[fs]), vu_ld(&vu->vf[ft])); break;
    default:
        return;
    }
    if (set_flags) vu_set_flags(vu, dest, r, fslot);
    vu_wr(vu, fd, (int)dest, r);
}

static int vu1_shadow;

PS2_INLINE void vu_do_xgkick(ps2_vu *vu, u32 vi) {
    if (!vu->mem || vu1_shadow) return;
    vu_stat_xgkick++;
    vu_stat_xgkick_qw += ps2_gif_kick(vu->mem, vu->mem_size, vi);
}

static int vu_qlat = -1;

static int vu_qlat_on(void) {
    if (vu_qlat < 0) {
        const char *e = getenv("PS2_VU_QLAT");
        vu_qlat = (e && *e) ? (strtol(e, NULL, 0) != 0) : 1;
        if (!vu_qlat)
            ps2_log("vu: Q/P latency disabled (PS2_VU_QLAT=0) -- DIV, SQRT, "
                    "RSQRT and the E-prefixed set publish immediately");
    }
    return vu_qlat;
}

PS2_INLINE void vu_q_commit(ps2_vu *vu) {
    if (vu->q_ready) { vu->q = vu->q_pend; vu->q_ready = 0; }
}

PS2_INLINE void vu_p_commit(ps2_vu *vu) {
    if (vu->p_ready) { vu->p = vu->p_pend; vu->p_ready = 0; }
}

PS2_INLINE void vu_fdiv_issue(ps2_vu *vu, u32 cyc, u32 lat, float v) {
    if (!vu_qlat_on()) { vu->q = v; vu->q_ready = 0; return; }
    vu_q_commit(vu);
    vu->q_pend = v;
    vu->q_ready = cyc + lat;
}

PS2_INLINE void vu_efu_issue(ps2_vu *vu, u32 cyc, u32 lat, float v) {
    if (!vu_qlat_on()) { vu->p = v; vu->p_ready = 0; return; }
    vu_p_commit(vu);
    vu->p_pend = v;
    vu->p_ready = cyc + lat;
}

static s64 vu_lower(ps2_vu *vu, u32 ins, u32 pc, u32 cyc) {
    u32 op = (ins >> 25) & 0x7Fu;
    s64 br = -1;
    if (ps2_diag_armed) {
        vu_low_hist[op & 0x7Fu]++;
        if (op == 0x40) {
            u32 l6 = ins & 0x3Fu, h5 = (ins >> 6) & 0x1Fu;
            vu_sp_lo6[l6]++;
            vu_sp_hi5[h5]++;
            vu_sp_pair[l6][h5]++;
        }
    }
    int itv = (int)((ins >> 16) & 0x1Fu);
    int isv = (int)((ins >> 11) & 0x1Fu);
    int it = itv & 15;
    int is = isv & 15;
    int id = (int)((ins >> 6) & 0xFu);
    u32 dest = (ins >> 21) & 0xFu;
    s32 imm11 = (s32)(ins & 0x7FFu);
    if (imm11 & 0x400) imm11 -= 0x800;

    switch (op) {
    case 0x00: {
        u32 addr = ((u32)vu->vi[is & 15] + (u32)imm11) * 16u;
        float r[4];
        memcpy(r, vu->mem + (addr & (vu->mem_size - 16u)), 16);
        vu_store(vu, itv, dest, r);
        return br;
    }
    case 0x01: {
        u32 addr = ((u32)vu->vi[it & 15] + (u32)imm11) * 16u;
        u32 off = addr & (vu->mem_size - 16u);
        for (int i = 0; i < 4; i++)
            if ((dest >> (3 - i)) & 1)
                memcpy(vu->mem + off + i * 4, &vu->vf[isv].f[i], 4);
        return br;
    }
    case 0x04: {
        u32 addr = ((u32)vu->vi[is & 15] + (u32)imm11) * 16u;
        int lane = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
        u16 v;
        memcpy(&v, vu->mem + ((addr + lane * 4u) & (vu->mem_size - 4u)), 2);
        if (it) vu->vi[it & 15] = v;
        return br;
    }
    case 0x05: {
        u32 addr = ((u32)vu->vi[is & 15] + (u32)imm11) * 16u;
        u32 v = vu->vi[it & 15];
        for (int i = 0; i < 4; i++)
            if ((dest >> (3 - i)) & 1)
                memcpy(vu->mem + ((addr + i * 4u) & (vu->mem_size - 4u)), &v, 4);
        return br;
    }
    case 0x08:
        if (it) vu->vi[it & 15] = (u16)(vu->vi[is & 15] +
                (u16)(((ins >> 10) & 0x7800u) | (ins & 0x7FFu)));
        return br;
    case 0x09:
        if (it) vu->vi[it & 15] = (u16)(vu->vi[is & 15] -
                (u16)(((ins >> 10) & 0x7800u) | (ins & 0x7FFu)));
        return br;
    case 0x10:
        vu->vi[1] =
            (u16)((vu->clip & 0xFFFFFFu) == (ins & 0xFFFFFFu) ? 1u : 0u);
        return br;
    case 0x11:
        vu->clip = ins & 0xFFFFFFu;
        return br;
    case 0x12:
        vu->vi[1] =
            (u16)((vu->clip & (ins & 0xFFFFFFu)) ? 1u : 0u);
        return br;
    case 0x13:
        vu->vi[1] =
            (u16)(((vu->clip | (ins & 0xFFFFFFu)) & 0xFFFFFFu) == 0xFFFFFFu
                  ? 1u : 0u);
        return br;
    case 0x14:
        if (it) vu->vi[it & 15] =
            (u16)((vu->status & 0xFFFu) == (ins & 0xFFFu) ? 1u : 0u);
        return br;
    case 0x15:
        vu->status = (vu->status & 0x3Fu) | (ins & 0xFC0u);
        return br;
    case 0x16:
        if (it) vu->vi[it & 15] = (u16)(vu->status & (ins & 0xFFFu));
        return br;
    case 0x17:
        if (it) vu->vi[it & 15] = (u16)((vu->status | (ins & 0xFFFu)) & 0xFFFu);
        return br;
    case 0x18:
        if (it) vu->vi[it & 15] =
            (u16)((vu->mac & 0xFFFFu) == vu->vi[is & 15] ? 1u : 0u);
        return br;
    case 0x1A:
        if (it) vu->vi[it & 15] = (u16)(vu->mac & vu->vi[is & 15]);
        return br;
    case 0x1B:
        if (it) vu->vi[it & 15] = (u16)(vu->mac | vu->vi[is & 15]);
        return br;
    case 0x1C:
        if (it) vu->vi[it & 15] = (u16)(vu->clip & 0xFFFu);
        return br;
    case 0x20:
        br = (s64)(pc + (u32)(imm11 * 8));
        if (ps2_diag_armed) vu_branch_seen(pc - 8u, 1);
        return br;
    case 0x21:
        if (it) vu->vi[it & 15] = (u16)(((pc + 8u) / 8u) & 0xFFFFu);
        br = (s64)(pc + (u32)(imm11 * 8));
        return br;
    case 0x24:
        br = (s64)((u32)vu->vi[is & 15] * 8u);
        return br;
    case 0x25:
        if (it) vu->vi[it & 15] = (u16)(((pc + 8u) / 8u) & 0xFFFFu);
        br = (s64)((u32)vu->vi[is & 15] * 8u);
        return br;
    case 0x28:
        { int t = (vu->vi[it & 15] == vu->vi[is & 15]) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    case 0x29:
        { int t = (vu->vi[it & 15] != vu->vi[is & 15]) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    case 0x2C:
        { int t = ((s16)vu->vi[is & 15] < 0) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    case 0x2D:
        { int t = ((s16)vu->vi[is & 15] > 0) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    case 0x2E:
        { int t = ((s16)vu->vi[is & 15] <= 0) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    case 0x2F:
        { int t = ((s16)vu->vi[is & 15] >= 0) ? 1 : 0;
          if (t) br = (s64)(pc + (u32)(imm11 * 8));
          if (ps2_diag_armed) vu_branch_seen(pc - 8u, t); }
        return br;
    default:
        break;
    }
    if (op == 0x40) {
        u32 low6 = ins & 0x3Fu;
        u32 sub  = ins & 0x7FFu;
        switch (low6) {
        case 0x30: if (id) vu->vi[id & 15] = (u16)(vu->vi[is & 15] + vu->vi[it & 15]); return br;
        case 0x31: if (id) vu->vi[id & 15] = (u16)(vu->vi[is & 15] - vu->vi[it & 15]); return br;
        case 0x32: {
            s32 imm5 = (s32)((ins >> 6) & 0x1Fu);
            if (imm5 & 0x10) imm5 -= 0x20;
            if (it) vu->vi[it & 15] = (u16)(vu->vi[is & 15] + (u16)(s16)imm5);
            return br;
        }
        case 0x34: if (id) vu->vi[id & 15] = vu->vi[is & 15] & vu->vi[it & 15]; return br;
        case 0x35: if (id) vu->vi[id & 15] = vu->vi[is & 15] | vu->vi[it & 15]; return br;
        default: break;
        }
        switch (sub) {
        case 0x33C: {
            float r[4];
            for (int i = 0; i < 4; i++) r[i] = vu->vf[isv].f[i];
            vu_store(vu, itv, dest, r);
            return br;
        }
        case 0x33D: {
            float r[4];
            r[0] = vu->vf[isv].f[1]; r[1] = vu->vf[isv].f[2];
            r[2] = vu->vf[isv].f[3]; r[3] = vu->vf[isv].f[0];
            vu_store(vu, itv, dest, r);
            return br;
        }
        case 0x37C:
        case 0x37E: {
            u32 a;
            float r[4];
            if (sub == 0x37E && is) vu->vi[is & 15] = (u16)(vu->vi[is & 15] - 1u);
            a = ((u32)vu->vi[is & 15] * 16u) & (vu->mem_size - 16u);
            memcpy(r, vu->mem + a, 16);
            vu_store(vu, itv, dest, r);
            if (sub == 0x37C && is) vu->vi[is & 15] = (u16)(vu->vi[is & 15] + 1u);
            return br;
        }
        case 0x37D:
        case 0x37F: {
            u32 a;
            int i;
            if (sub == 0x37F && it) vu->vi[it & 15] = (u16)(vu->vi[it & 15] - 1u);
            a = ((u32)vu->vi[it & 15] * 16u) & (vu->mem_size - 16u);
            for (i = 0; i < 4; i++)
                if ((dest >> (3 - i)) & 1)
                    memcpy(vu->mem + a + i * 4, &vu->vf[isv].f[i], 4);
            if (sub == 0x37D && it) vu->vi[it & 15] = (u16)(vu->vi[it & 15] + 1u);
            return br;
        }
        case 0x3BC:
            vu_fdiv_issue(vu, cyc, 7,
                          ps2_fdiv(vu->vf[isv].f[(ins >> 21) & 3u],
                                   vu->vf[itv].f[(ins >> 23) & 3u]));
            return br;
        case 0x3BD:
            vu_fdiv_issue(vu, cyc, 7,
                          ps2_fsqrt(vu->vf[itv].f[(ins >> 23) & 3u]));
            return br;
        case 0x3BE:
            vu_fdiv_issue(vu, cyc, 13,
                          ps2_frsqrt(vu->vf[isv].f[(ins >> 21) & 3u],
                                     vu->vf[itv].f[(ins >> 23) & 3u]));
            return br;
        case 0x3BF:
            vu_q_commit(vu);
            return br;
        case 0x3FC:
            if (it) vu->vi[it & 15] =
                (u16)(vu->vf[isv].u[(ins >> 21) & 3u] & 0xFFFFu);
            return br;
        case 0x3FD: {
            float r[4];
            s32 v = (s32)(s16)vu->vi[is & 15];
            int i;
            for (i = 0; i < 4; i++) memcpy(&r[i], &v, 4);
            vu_store(vu, itv, dest, r);
            return br;
        }
        case 0x3FE: {
            u32 a = ((u32)vu->vi[is & 15] * 16u) & (vu->mem_size - 16u);
            int lane = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
            u16 v;
            memcpy(&v, vu->mem + a + (u32)lane * 4u, 2);
            if (it) vu->vi[it & 15] = v;
            return br;
        }
        case 0x3FF: {
            u32 a = ((u32)vu->vi[is & 15] * 16u) & (vu->mem_size - 16u);
            u32 v = vu->vi[it & 15];
            int i;
            for (i = 0; i < 4; i++)
                if ((dest >> (3 - i)) & 1) memcpy(vu->mem + a + i * 4, &v, 4);
            return br;
        }
        case 0x43C:
            vu->r = ((vu->r << 1) | (((vu->r >> 22) ^ (vu->r >> 21)) & 1u))
                    & 0x7FFFFFu;
            __attribute__((fallthrough));
        case 0x43D: {
            float r[4];
            u32 bits = 0x3F800000u | (vu->r & 0x7FFFFFu);
            int i;
            for (i = 0; i < 4; i++) memcpy(&r[i], &bits, 4);
            vu_store(vu, itv, dest, r);
            return br;
        }
        case 0x43E:
            vu->r = vu->vf[isv].u[(ins >> 21) & 3u] & 0x7FFFFFu;
            return br;
        case 0x43F:
            vu->r = (vu->r ^ vu->vf[isv].u[(ins >> 21) & 3u]) & 0x7FFFFFu;
            return br;
        case 0x67C: {
            float r[4];
            int i;
            for (i = 0; i < 4; i++) r[i] = vu->p;
            vu_store(vu, itv, dest, r);
            return br;
        }
        case 0x6BC:
            if (it) vu->vi[it & 15] = (u16)vu->top;
            return br;
        case 0x6BD:
            if (it) vu->vi[it & 15] = (u16)vu->itop;
            return br;
        case 0x6FC:
            vu_do_xgkick(vu, (u32)vu->vi[is & 15]);
            return br;
        case 0x73C: case 0x73D: {
            float x = vu->vf[isv].f[0], y = vu->vf[isv].f[1], z = vu->vf[isv].f[2];
            float t = x * x + y * y + z * z;
            vu_efu_issue(vu, cyc, sub == 0x73D ? 18 : 11,
                         (sub == 0x73D) ? (t == 0.0f ? 3.4028235e38f : 1.0f / t) : t);
            return br;
        }
        case 0x73E: case 0x73F: {
            float x = vu->vf[isv].f[0], y = vu->vf[isv].f[1], z = vu->vf[isv].f[2];
            float t = ps2_sqrtf(x * x + y * y + z * z);
            vu_efu_issue(vu, cyc, sub == 0x73F ? 24 : 18,
                         (sub == 0x73F) ? (t == 0.0f ? 3.4028235e38f : 1.0f / t) : t);
            return br;
        }
        case 0x77C:
            vu_efu_issue(vu, cyc, 54,
                         ps2_atan2f(vu->vf[isv].f[1], vu->vf[isv].f[0]));
            return br;
        case 0x77D:
            vu_efu_issue(vu, cyc, 54,
                         ps2_atan2f(vu->vf[isv].f[2], vu->vf[isv].f[0]));
            return br;
        case 0x77E:
            vu_efu_issue(vu, cyc, 12,
                         vu->vf[isv].f[0] + vu->vf[isv].f[1]
                       + vu->vf[isv].f[2] + vu->vf[isv].f[3]);
            return br;
        case 0x7BC: {
            float a = vu->vf[isv].f[(ins >> 21) & 3u];
            vu_efu_issue(vu, cyc, 12, ps2_sqrtf(a < 0.0f ? -a : a));
            return br;
        }
        case 0x7BD: {
            float a = vu->vf[isv].f[(ins >> 21) & 3u];
            float r = ps2_sqrtf(a < 0.0f ? -a : a);
            vu_efu_issue(vu, cyc, 18,
                         (r == 0.0f) ? 3.4028235e38f : 1.0f / r);
            return br;
        }
        case 0x7BE: {
            float a = vu->vf[isv].f[(ins >> 21) & 3u];
            vu_efu_issue(vu, cyc, 12,
                         (a == 0.0f) ? 3.4028235e38f : 1.0f / a);
            return br;
        }
        case 0x7BF:
            vu_p_commit(vu);
            return br;
        case 0x7FC:
            vu_efu_issue(vu, cyc, 29,
                         ps2_sinf(vu->vf[isv].f[(ins >> 21) & 3u]));
            return br;
        case 0x7FD:
            vu_efu_issue(vu, cyc, 54,
                         ps2_atanf(vu->vf[isv].f[(ins >> 21) & 3u]));
            return br;
        case 0x7FE:
            vu_efu_issue(vu, cyc, 44,
                         ps2_expf(-vu->vf[isv].f[(ins >> 21) & 3u]));
            return br;
        default:
            break;
        }
    }
    for (u32 k = 0; k < vu_unknown_n; k++)
        if (vu_unknown_lower[k] == op) return br;
    if (vu_unknown_n < 64) {
        vu_unknown_lower[vu_unknown_n++] = op;
        ps2_log("vu: unimplemented lower opcode 0x%02X (insn %08X)", op, ins);
    }
    return br;
}

static u32 vu_watch_pc = 0xFFFFFFFFu;
static u32 vu_watch_left;
static void vu_watch_init(void) {
    static int done;
    const char *e;
    if (done) return;
    done = 1;
    e = getenv("PS2_VU_WATCH");
    if (!e) return;
    vu_watch_pc = (u32)strtoul(e, NULL, 0) * 8u;
    { const char *c = strchr(e, ','); vu_watch_left = c ? (u32)strtoul(c + 1, NULL, 0) : 24u; }
    ps2_log("vu1: watching instruction %u (%u times)", vu_watch_pc / 8u, vu_watch_left);
}

static void vu_watch_hit(const ps2_vu *vu, u32 pc) {
    char line[256];
    int n = 0, i;
    for (i = 0; i < 16; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %u:%u", i, vu->vi[i]);
    line[n] = 0;
    ps2_log("vu1 watch @%u  MAC=%04X STATUS=%03X CLIP=%06X  VF01=%g %g %g %g",
            pc / 8u, vu->mac, vu->status, vu->clip,
            (double)vu->vf[1].f[0], (double)vu->vf[1].f[1],
            (double)vu->vf[1].f[2], (double)vu->vf[1].f[3]);
    ps2_log("            VI:%s", line);
}

#if defined(__has_include)
#  if __has_include("ps2_vu1_progs.inc")
#    define VU1_HAVE_RECOMP 1
#  endif
#endif

#ifdef VU1_HAVE_RECOMP
#include "ps2_vu1_progs.inc"
#endif

static int vu1_recomp_off = -1;
static int (*vu1_resident_fn)(ps2_vu *, u32, u32, u32 *, u32 *, u32 *);
static u64 vu1_recomp_runs, vu1_recomp_bails, vu1_interp_runs;

static int vu1_recomp_enabled(void) {
#ifndef VU1_HAVE_RECOMP
    return 0;
#else
    if (vu1_recomp_off < 0) {
        const char *e = getenv("PS2_VU_RECOMP");
        vu1_recomp_off = (e && *e && *e == '0') ? 1 : 0;
        if (vu1_recomp_off)
            ps2_log("vu1: recompiled microprograms disabled (PS2_VU_RECOMP=0)");
        else
            ps2_log("vu1: %d recompiled microprograms available",
                    VU1_RECOMP_N);
    }
    return !vu1_recomp_off;
#endif
}

#define VU1_MEMO_N 64
static struct { u64 crc; int idx; } vu1_memo[VU1_MEMO_N];
static u32 vu1_memo_n;
static u64 vu1_memo_hits, vu1_memo_misses;

#ifdef VU1_HAVE_RECOMP
static int vu1_sig_match(const u8 *m, int i) {
    const unsigned short *pcs = vu1_recomp_table[i].pc;
    const unsigned int *w = vu1_recomp_table[i].w;
    unsigned k, n = vu1_recomp_table[i].npairs;
    for (k = 0; k < n; k++) {
        u32 lo, up;
        memcpy(&lo, m + pcs[k], 4);
        memcpy(&up, m + pcs[k] + 4, 4);
        if (lo != w[2 * k] || up != w[2 * k + 1]) return 0;
    }
    return 1;
}
#endif

static void vu1_resident_update(ps2_vu *vu) {
    u32 k;
    vu_prog_crc = vu_hash(vu->micro, vu->micro_size);
    vu_prog_dirty = 0;
    vu1_resident_fn = NULL;
#ifdef VU1_HAVE_RECOMP
    for (k = 0; k < vu1_memo_n; k++)
        if (vu1_memo[k].crc == vu_prog_crc) {
            vu1_memo_hits++;
            if (vu1_memo[k].idx >= 0)
                vu1_resident_fn = vu1_recomp_table[vu1_memo[k].idx].fn;
            return;
        }
    vu1_memo_misses++;
    {   int i, found = -1;
        for (i = 0; i < VU1_RECOMP_N; i++)
            if (vu1_sig_match(vu->micro, i)) { found = i; break; }
        if (found >= 0) vu1_resident_fn = vu1_recomp_table[found].fn;
        if (vu1_memo_misses <= 8)
            ps2_log("vu1: image %016llx -> %s", (unsigned long long)vu_prog_crc,
                    found >= 0 ? "matched a recompiled program"
                               : "no recompiled program matches");
        if (vu1_memo_n < VU1_MEMO_N) {
            vu1_memo[vu1_memo_n].crc = vu_prog_crc;
            vu1_memo[vu1_memo_n].idx = found;
            vu1_memo_n++;
        }
    }
#else
    (void)k;
#endif
}

static vu_prog *vu_census_enter(ps2_vu *vu, u32 start) {
    vu_prog *p;
    u32 i;
    if (!vu_census_enabled() || vu != &vu1 || !vu->micro) return NULL;
    if (vu_prog_dirty) vu1_resident_update(vu);
    for (i = 0; i < vu_prog_n; i++)
        if (vu_progs[i].crc == vu_prog_crc) { p = &vu_progs[i]; goto found; }
    if (vu_prog_n >= VU_PROG_N) { vu_prog_dropped++; return NULL; }
    p = &vu_progs[vu_prog_n++];
    p->crc = vu_prog_crc;
    p->image_size = vu->micro_size;
    p->image = (u8 *)malloc(vu->micro_size);
    if (!p->image) { vu_prog_n--; vu_prog_dropped++; return NULL; }
    memcpy(p->image, vu->micro, vu->micro_size);
    p->lo = vu->micro_size; p->hi = 0;
    for (i = 0; i < vu->micro_size; i += 8) {
        u64 pair;
        memcpy(&pair, p->image + i, 8);
        if (!pair) continue;
        if (i < p->lo) p->lo = i;
        if (i + 8u > p->hi) p->hi = i + 8u;
    }
    if (p->lo > p->hi) p->lo = p->hi = 0;
found:
    p->activations++;
    for (i = 0; i < p->n_entries; i++)
        if (p->entries[i] == start) { p->entry_n[i]++; return p; }
    if (p->n_entries < 32) {
        p->entries[p->n_entries] = start;
        p->entry_n[p->n_entries] = 1;
        p->n_entries++;
    }
    return p;
}

typedef struct {
    ps2_vf vf[32], acc;
    u16 vi[16];
    float q, p, i;
    u32 r, status, mac, clip;
    struct { u32 mac, status; int valid; } fpipe[4];
    u32 fslot, top, itop, cmsar, tpc, cc;
    float q_pend, p_pend;
    u32 q_ready, p_ready;
    u8 mem[PS2_VU1_MEM_SIZE];
} vu1_snap;

static void vu1_snap_take(const ps2_vu *vu, vu1_snap *s) {
    memcpy(s->vf, vu->vf, sizeof s->vf);
    s->acc = vu->acc;
    memcpy(s->vi, vu->vi, sizeof s->vi);
    s->q = vu->q; s->p = vu->p; s->i = vu->i;
    s->r = vu->r; s->status = vu->status; s->mac = vu->mac; s->clip = vu->clip;
    memcpy(s->fpipe, vu->fpipe, sizeof s->fpipe);
    s->fslot = vu->fslot; s->top = vu->top; s->itop = vu->itop;
    s->cmsar = vu->cmsar; s->tpc = vu->tpc; s->cc = vu->cc;
    s->q_pend = vu->q_pend; s->p_pend = vu->p_pend;
    s->q_ready = vu->q_ready; s->p_ready = vu->p_ready;
    if (vu->mem) memcpy(s->mem, vu->mem, vu->mem_size);
}

static void vu1_snap_put(ps2_vu *vu, const vu1_snap *s) {
    memcpy(vu->vf, s->vf, sizeof s->vf);
    vu->acc = s->acc;
    memcpy(vu->vi, s->vi, sizeof s->vi);
    vu->q = s->q; vu->p = s->p; vu->i = s->i;
    vu->r = s->r; vu->status = s->status; vu->mac = s->mac; vu->clip = s->clip;
    memcpy(vu->fpipe, s->fpipe, sizeof s->fpipe);
    vu->fslot = s->fslot; vu->top = s->top; vu->itop = s->itop;
    vu->cmsar = s->cmsar; vu->tpc = s->tpc; vu->cc = s->cc;
    vu->q_pend = s->q_pend; vu->p_pend = s->p_pend;
    vu->q_ready = s->q_ready; vu->p_ready = s->p_ready;
    if (vu->mem) memcpy(vu->mem, s->mem, vu->mem_size);
}

static int vu1_verify_on = -1;
static u64 vu1_verify_runs, vu1_verify_bad;
static vu1_snap vu1_pre, vu1_interp_res, vu1_recomp_res;

static int vu1_verify_enabled(void) {
    if (vu1_verify_on < 0) {
        const char *e = getenv("PS2_VU_VERIFY");
        vu1_verify_on = (e && *e && *e != '0') ? 1 : 0;
        if (vu1_verify_on)
            ps2_log("vu1: differential verify armed -- every activation runs "
                    "interpreted AND recompiled, and the two states are "
                    "compared.  This is a correctness mode; it is slow.");
    }
    return vu1_verify_on;
}

static int vu1_snap_diff(const vu1_snap *a, const vu1_snap *b, u32 start) {
    int i, bad = 0;
#define DIFF(fmt, what, x, y) \
    do { if ((x) != (y)) { if (!bad++) ps2_log( \
        "vu1 VERIFY entry %04X: %s interp " fmt " recomp " fmt, \
        start, what, (x), (y)); } } while (0)
    for (i = 0; i < 32; i++) {
        int k;
        for (k = 0; k < 4; k++)
            if (a->vf[i].u[k] != b->vf[i].u[k]) {
                if (!bad++)
                    ps2_log("vu1 VERIFY entry %04X: VF%02d.%c interp %08X "
                            "recomp %08X", start, i, "xyzw"[k],
                            a->vf[i].u[k], b->vf[i].u[k]);
            }
    }
    for (i = 0; i < 16; i++) DIFF("%u", "VI", a->vi[i], b->vi[i]);
    DIFF("%08X", "MAC", a->mac, b->mac);
    DIFF("%08X", "status", a->status, b->status);
    DIFF("%08X", "clip", a->clip, b->clip);
    DIFF("%08X", "tpc", a->tpc, b->tpc);
    DIFF("%u", "q_ready", a->q_ready, b->q_ready);
    if (memcmp(a->mem, b->mem, sizeof a->mem) != 0) {
        u32 k;
        for (k = 0; k < sizeof a->mem; k++)
            if (a->mem[k] != b->mem[k]) {
                if (!bad++)
                    ps2_log("vu1 VERIFY entry %04X: VU mem %04X interp %02X "
                            "recomp %02X", start, k, a->mem[k], b->mem[k]);
                break;
            }
    }
#undef DIFF
    return bad;
}

static int vu1_needs_interp = -1;

static int vu1_diag_wants_interp(void) {
    if (vu1_needs_interp < 0) {
        vu1_needs_interp = (getenv("PS2_VU_WATCH") || getenv("PS2_VU_PROFILE")
                            || getenv("PS2_TRACE_VU")) ? 1 : 0;
        if (vu1_needs_interp)
            ps2_log("vu1: a VU diagnostic is set (PS2_VU_WATCH / "
                    "PS2_VU_PROFILE / PS2_TRACE_VU), so microprograms are "
                    "interpreted rather than run from recompiled code");
    }
    return vu1_needs_interp;
}

static int vu1_try_recomp(ps2_vu *vu, u32 *pc, u32 *guard, u32 *fslot,
                          u32 limit, int diag, int *stopped) {
    int r;
    (void)diag;
    if (vu != &vu1 || !vu->micro || !vu1_recomp_enabled()) return 0;
    if (vu1_diag_wants_interp()) return 0;
    if (vu_prog_dirty) vu1_resident_update(vu);
    if (!vu1_resident_fn) { vu1_interp_runs++; return 0; }
    r = vu1_resident_fn(vu, *pc, limit, pc, guard, fslot);
    if (r == 0) { vu1_interp_runs++; return 0; }
    if (r == 2) { vu1_recomp_bails++; return 0; }
    *stopped = r == 1;
    if (*stopped) vu1_recomp_runs++;
    return 1;
}

void ps2_vu_recomp_report(void) {
    u64 tot = vu1_recomp_runs + vu1_interp_runs + vu1_recomp_bails;
    if (vu1_verify_runs)
        ps2_log("vu1 verify: %llu activations run BOTH ways and compared, "
                "%llu mismatched",
                (unsigned long long)vu1_verify_runs,
                (unsigned long long)vu1_verify_bad);
    if (!tot) return;
    ps2_log("vu1 recompiled: %llu runs completed, %llu handed back at an "
            "unknown JR, %llu interpreted (%.1f%% recompiled)",
            (unsigned long long)vu1_recomp_runs,
            (unsigned long long)vu1_recomp_bails,
            (unsigned long long)vu1_interp_runs,
            100.0 * (double)vu1_recomp_runs / (double)tot);
    ps2_log("vu1 program identity: %u distinct images seen, %llu memo hits, "
            "%llu full signature comparisons",
            vu1_memo_n, (unsigned long long)vu1_memo_hits,
            (unsigned long long)vu1_memo_misses);
}

static void vu_run(ps2_vu *vu, u32 start) {
    u32 pc = start & (vu->micro_size - 8u);
    vu_prog *census = vu_census_enter(vu, pc);
    vu_watch_init();
    u32 guard = 0;
    u64 kicks_before = vu_stat_xgkick;
    int stop = 0;
    int trace = 0;
    static int budget = -1;
    static u64 traced;
    if (budget < 0) {
        const char *e = getenv("PS2_TRACE_VU");
        budget = e ? (int)strtol(e, NULL, 0) : 0;
    }
    if (budget > 0 && ps2_diag_armed && vu != &ps2_cpu.vu0
        && traced < (u64)budget) {
        traced++;
        trace = 1;
        ps2_log("vu1: run %llu starts at %04X (instruction %u)",
                (unsigned long long)traced, start, start / 8u);
    }
    vu->running = 1;
    { PS2_PHASE_BEGIN(PS2_PH_VU);
    {
        s64 br = -1;
        s64 pending = -1;
        int ebit = 0;
        u32 i_next = 0;
        int i_pending = 0;
        const u8 *micro = vu->micro;
        u32 pcmask = vu->micro_size - 8u;
        u32 limit = vu == &ps2_cpu.vu0 ? vu0_run_limit : vu_run_limit;
        u32 fslot = 0;
        int watch = (vu_watch_left != 0u) && ps2_diag_armed;
        int prof = vu_profile_on() && vu != &ps2_cpu.vu0;
        int diag = ps2_diag_armed;
        u32 entry_pc = pc;
        int verify = 0;
        if (PS2_UNLIKELY(vu1_verify_enabled()) && vu == &vu1 && vu->micro) {
            if (vu_prog_dirty) vu1_resident_update(vu);
            if (vu1_resident_fn) { vu1_snap_take(vu, &vu1_pre); verify = 1; }
        }
#if VU_SIMD
        u32 mxcsr = _mm_getcsr();
        _mm_setcsr(mxcsr | 0x8000u);
#endif
    if (verify || !vu1_try_recomp(vu, &pc, &guard, &fslot, limit, diag, &stop))
    while (!stop && guard < limit) {
            guard++;
            u32 lo, up;
            fslot = (u32)(guard - 1u) & 3u;
            if (vu->fpipe[fslot].valid) {
                vu->mac = vu->fpipe[fslot].mac;
                vu->status = vu->fpipe[fslot].status;
                vu->fpipe[fslot].valid = 0;
            }
            if (PS2_UNLIKELY(vu->q_ready) && guard >= vu->q_ready) vu_q_commit(vu);
            if (PS2_UNLIKELY(vu->p_ready) && guard >= vu->p_ready) vu_p_commit(vu);
            if (PS2_UNLIKELY(watch) && pc == vu_watch_pc && vu_watch_left) {
                vu_watch_left--;
                watch = vu_watch_left != 0u;
                vu_watch_hit(vu, pc);
            }
            memcpy(&lo, micro + pc, 4);
            memcpy(&up, micro + pc + 4, 4);
            if (PS2_UNLIKELY(prof)) {
                u32 f = up & 0x3Fu;
                vu_pc_hist[(pc >> 3) & (VU_PROF_SLOTS - 1u)]++;
                vu_up_hist[((f & 0x3Cu) == 0x3Cu)
                           ? (0x80u | ((up & 3u) | ((up >> 4) & 0x7Cu)))
                           : f]++;
                if (!(up & 0x80000000u) && ((lo >> 25) & 0x7Fu) == 0x40u)
                    vu_lo_sub_hist[lo & 0x7FFu]++;
            }
            if (PS2_UNLIKELY(trace) && guard <= 64)
                ps2_log("vu1:   %04X  lo=%08X up=%08X  lo_op=%02X lo11=%03X%s%s",
                        pc, lo, up, (lo >> 25) & 0x7Fu, lo & 0x7FFu,
                        (up & 0x40000000u) ? "  [E]" : "",
                        (up & 0x80000000u) ? "  [I]" : "");
            pc = (pc + 8) & pcmask;
            br = -1;
            {
                u32 uf = up & 0x3Fu;
                int up_nop = (uf & 0x3Cu) == 0x3Cu
                             && ((up & 3u) | ((up >> 4) & 0x7Cu)) == 0x2Fu;
                if (up & 0x80000000u) {
                    i_next = lo;
                    i_pending = 1;
                } else {
                    int lo_nop = ((lo >> 25) & 0x7Fu) == 0x40u
                                 && (lo & 0x7FFu) == 0x33Cu
                                 && ((lo >> 21) & 0xFu) == 0u;
                    if (!lo_nop || PS2_UNLIKELY(diag))
                        br = vu_lower(vu, lo, pc, guard);
                }
                if (!up_nop) vu_upper(vu, up & 0x7FFFFFFFu, fslot);
            }
            if (i_pending) { memcpy(&vu->i, &i_next, 4); i_pending = 0; }
            if (ebit) { if (--ebit == 0) stop = 1; }
            else if (up & 0x40000000u) ebit = 1;
            if (pending >= 0) {
                pc = (u32)pending & pcmask;
                pending = -1;
            }
            if (br >= 0) pending = br;
        }
    vu->fslot = fslot;
    if (PS2_UNLIKELY(verify)) {
        u32 rpc = entry_pc, rg = 0, rf = 0;
        int bad;
        vu1_snap_take(vu, &vu1_interp_res);
        vu1_snap_put(vu, &vu1_pre);
        vu1_shadow = 1;
        (void)vu1_resident_fn(vu, entry_pc, limit, &rpc, &rg, &rf);
        vu1_shadow = 0;
        vu->fslot = rf;
        vu1_snap_take(vu, &vu1_recomp_res);
        vu1_verify_runs++;
        bad = vu1_snap_diff(&vu1_interp_res, &vu1_recomp_res, entry_pc);
        if (rpc != pc || rg != guard) {
            if (!bad)
                ps2_log("vu1 VERIFY entry %04X: interp stopped at %04X after "
                        "%u instructions, recomp at %04X after %u",
                        entry_pc, pc, guard, rpc, rg);
            bad++;
        }
        if (bad && vu1_verify_bad < 32) vu1_verify_bad++;
        else if (bad) vu1_verify_bad++;
        vu1_snap_put(vu, &vu1_interp_res);
        vu->fslot = fslot;
    }
#if VU_SIMD
    _mm_setcsr(mxcsr);
#endif
    }
    {
        u32 k;
        for (k = 3; k >= 1; k--) {
            u32 slot = (u32)(guard - k) & 3u;
            if (!vu->fpipe[slot].valid) continue;
            vu->mac = vu->fpipe[slot].mac;
            vu->status = vu->fpipe[slot].status;
            vu->fpipe[slot].valid = 0;
        }
        vu_q_commit(vu);
        vu_p_commit(vu);
    }
    vu->tpc = pc;
    if (census) {
        census->insns += guard;
        census->kicks += vu_stat_xgkick - kicks_before;
    }
    vu_stat_insns += guard;
    if (vu == &vu1) {
        speed_vu1_runs++; speed_vu1_insns+=guard;
        if (!stop) speed_vu1_guards++;
    }
    if (PS2_UNLIKELY(vu_profile == 1) && vu != &ps2_cpu.vu0) {
        u32 slot = (start >> 3) & (VU_PROF_SLOTS - 1u);
        vu_prog_insns[slot] += guard;
        vu_prog_runs[slot]++;
    }
    if (vu != &ps2_cpu.vu0) {
        u32 b = 0, g = guard;
        while (g > 1u && b < 23u) { g >>= 1; b++; }
        vu_runlen_hist[b]++;
        if (vu_stat_xgkick != kicks_before) vu_runs_with_kick++;
    }
    if (!stop) {
        vu->tpc = 0;
        vu_stat_guard_hits++;
        if (vu_stat_guard_hits <= 4) {
            u32 a;
            ps2_log("vu1: RUNAWAY run started at %04X, still at %04X after "
                    "%u instructions and was stopped", start, pc, guard);
            for (a = (pc >= 48u ? pc - 48u : 0u); a <= pc + 24u
                 && a + 8u <= vu->micro_size; a += 8u) {
                u32 l, u;
                memcpy(&l, vu->micro + a, 4);
                memcpy(&u, vu->micro + a + 4, 4);
                ps2_log("   %04X lo=%08X up=%08X  lo_prim=%02X lo11=%03X%s%s%s",
                        a, l, u, (l >> 25) & 0x7Fu, l & 0x7FFu,
                        a == pc ? "   <-- here" : "",
                        (u & 0x40000000u) ? "  [E]" : "",
                        (u & 0x80000000u) ? "  [I]" : "");
            }
        }
    }
    if (trace)
        ps2_log("vu1: run ended after %u instructions (%s), tpc=%04X", guard,
                stop ? "stopped" : "hit the instruction guard", pc);
    PS2_PHASE_END(PS2_PH_VU); }
    vu->running = 0;
}

void ps2_vu0_callms(ps2_ctx *ctx, u32 addr) {
    ps2_vu0_advance(ctx, 5);
    vu_run(&ctx->vu0, addr);
}

#define VIF_UNPACK_CENSUS_N 48
static struct {
    u32 cmd, cl, wl, num, m, flg, addr;
    u64 n;
} vif_unpack_cens[VIF_UNPACK_CENSUS_N];
static unsigned vif_unpack_cens_n;

static void vif_unpack_census(u32 cmd, u32 cl, u32 wl, u32 num, u32 m,
                              u32 flg, u32 addr) {
    unsigned i;
    for (i = 0; i < vif_unpack_cens_n; i++) {
        if (vif_unpack_cens[i].cmd == cmd && vif_unpack_cens[i].cl == cl
            && vif_unpack_cens[i].wl == wl && vif_unpack_cens[i].num == num
            && vif_unpack_cens[i].m == m && vif_unpack_cens[i].flg == flg
            && vif_unpack_cens[i].addr == addr) {
            vif_unpack_cens[i].n++;
            return;
        }
    }
    if (vif_unpack_cens_n >= VIF_UNPACK_CENSUS_N) return;
    i = vif_unpack_cens_n++;
    vif_unpack_cens[i].cmd = cmd; vif_unpack_cens[i].cl = cl;
    vif_unpack_cens[i].wl = wl;   vif_unpack_cens[i].num = num;
    vif_unpack_cens[i].m = m;     vif_unpack_cens[i].flg = flg;
    vif_unpack_cens[i].addr = addr;
    vif_unpack_cens[i].n = 1;
}

void ps2_vif_unpack_report(void) {
    static const char *fmt[16] = {
        "S-32", "S-16", "S-8", "?", "V2-32", "V2-16", "V2-8", "?",
        "V3-32", "V3-16", "V3-8", "?", "V4-32", "V4-16", "V4-8", "V4-5"
    };
    unsigned i;
    if (!vif_unpack_cens_n) return;
    ps2_log("VIF1 UNPACK census (%u distinct):", vif_unpack_cens_n);
    for (i = 0; i < vif_unpack_cens_n; i++)
        ps2_log("   %-6s cmd=%02X num=%-3u cl=%-3u wl=%-3u %s %s addr=%u  x%llu",
                fmt[vif_unpack_cens[i].cmd & 15u], vif_unpack_cens[i].cmd,
                vif_unpack_cens[i].num, vif_unpack_cens[i].cl,
                vif_unpack_cens[i].wl,
                vif_unpack_cens[i].m ? "mask" : "----",
                vif_unpack_cens[i].flg ? "+TOPS" : "     ",
                vif_unpack_cens[i].addr,
                (unsigned long long)vif_unpack_cens[i].n);
}

static u32 vif_unpack_bits(const ps2_vif *v) {
    return (32u >> v->unpack_vl) * (v->unpack_vn + 1u);
}

static int vif_unpack_is_fill(const ps2_vif *v) {
    u32 cl = v->unpack_cl, wl = v->unpack_wl;
    if (cl >= wl) return 0;
    return (v->unpack_written % wl) >= cl;
}

static u32 vif_unpack_dest(const ps2_vif *v) {
    u32 n = v->unpack_written, cl = v->unpack_cl, wl = v->unpack_wl;
    if (cl >= wl) return v->unpack_base + cl * (n / wl) + (n % wl);
    return v->unpack_base + n;
}

static void vif_unpack_emit(ps2_vif *v, ps2_vu *vu, const u32 *data, int have) {
    u32 addr = (vif_unpack_dest(v) * 16u) & (vu->mem_size - 16u);
    u32 cycle = v->unpack_written % (v->unpack_wl ? v->unpack_wl : 1u);
    u32 row_sel = cycle < 3u ? cycle : 3u;
    int i;
    for (i = 0; i < 4; i++) {
        u32 m = v->unpack_m ? ((v->mask >> (row_sel * 8u + (u32)i * 2u)) & 3u) : 0u;
        u32 val;
        if (m == 3u) continue;
        if (m == 1u) { val = v->row[i]; }
        else if (m == 2u) { val = v->col[row_sel]; }
        else {
            if (i >= have) continue;
            val = data[i];
            if (v->mode && v->unpack_vl != 3u) {
                val = val + v->row[i];
                if (v->mode == 2u) v->row[i] = val;
            }
        }
        memcpy(vu->mem + addr + (u32)i * 4u, &val, 4);
    }
    v->unpack_written++;
    if (v->unpack_num) v->unpack_num--;
    if (v->num) v->num--;
}

static void vif_unpack_fill(ps2_vif *v, ps2_vu *vu) {
    static const u32 none[4] = {0, 0, 0, 0};
    vif_unpack_emit(v, vu, none, 0);
}

static void vif_unpack_element(ps2_vif *v, ps2_vu *vu) {
    u32 out[4] = {0, 0, 0, 0};
    u32 comps = v->unpack_vn + 1u;
    u32 i;
    int have;
    if (v->unpack_vl == 3u) {
        u32 h = v->elem[0] & 0xFFFFu;
        out[0] = (h & 0x1Fu) << 3;
        out[1] = ((h >> 5) & 0x1Fu) << 3;
        out[2] = ((h >> 10) & 0x1Fu) << 3;
        out[3] = ((h >> 15) & 1u) << 7;
        have = 4;
    } else {
        for (i = 0; i < (comps == 3u ? 4u : comps); i++) {
            u32 raw = v->elem[i];
            switch (v->unpack_vl) {
            case 0: out[i] = raw; break;
            case 1: out[i] = v->unpack_usn ? (raw & 0xFFFFu)
                                           : (u32)(s32)(s16)(u16)raw; break;
            default: out[i] = v->unpack_usn ? (raw & 0xFFu)
                                            : (u32)(s32)(s8)(u8)raw; break;
            }
        }
        if (comps == 1u) { out[1] = out[2] = out[3] = out[0]; have = 4; }
        else if (comps == 2u) {
            out[2] = out[0];
            out[3] = v->unpack_vl == 0u && (v->unpack_align & 1u) ? 0u : out[1];
            have = 4;
        } else have = 4;
    }
    vif_unpack_emit(v, vu, out, have);
}

static int vif_v3_zero_w(const ps2_vif *v) {
    u32 n = v->unpack_index, a = v->unpack_align;
    if (v->unpack_vl == 0u) return (n & 1u) != (a & 1u);
    if (v->unpack_vl == 1u)
        return !((n + 1u) & 1u) && ((((n + 1u) / 4u + 1u + 4u - a) & 3u) == 0);
    return n + 1u != (a & 1u);
}
static void vif_unpack_drain(ps2_vif *v, ps2_vu *vu) {
    u32 cbits = (v->unpack_vl == 3u) ? 16u : (32u >> v->unpack_vl);
    u32 need = (v->unpack_vl == 3u) ? 1u : (v->unpack_vn + 1u);
    while (v->unpack_num) {
        if (v->elem_n == 0 && vif_unpack_is_fill(v)) { vif_unpack_fill(v, vu); continue; }
        if (v->elem_n == need) {
            if (need == 3u) {
                if (!vif_v3_zero_w(v) && v->nbits < cbits) return;
                v->elem[3] = vif_v3_zero_w(v) ? 0u : (u32)v->bits;
            }
            v->elem_n = 0;
            vif_unpack_element(v, vu);
            v->unpack_index++;
            continue;
        }
        if (v->nbits < cbits) return;
        v->elem[v->elem_n++] = (u32)v->bits;
        v->bits >>= cbits;
        v->nbits -= cbits;
    }
}

static void vif_unpack_stream(ps2_vif *v, ps2_vu *vu, u32 w) {
    v->bits |= (u64)w << v->nbits;
    v->nbits += 32u;
    vif_unpack_drain(v, vu);
    if (!v->wait) {
        if (v->nbits && v->elem_n == 0) { v->bits = 0; v->nbits = 0; }
        while (v->unpack_num && vif_unpack_is_fill(v)) vif_unpack_fill(v, vu);
    }
}

static void vif_unpack_word(ps2_vif *v, ps2_vu *vu, u32 w) {
    PS2_PHASE_BEGIN(PS2_PH_UNPACK);
    vif_unpack_stream(v, vu, w);
    PS2_PHASE_END(PS2_PH_UNPACK);
}

static void vif_activate(ps2_vif *v, ps2_vu *vu) {
    v->itop = v->itops;
    v->top = v->tops;
    v->dbf ^= 1u;
    v->tops = v->base + (v->dbf ? v->offset : 0u);
    vu->top = v->top;
    vu->itop = v->itop;
    if (v->is_vif1) {
        u32 hdr[4] = {0, 0, 0, 0};
        if (vu->mem)
            memcpy(hdr, vu->mem + ((v->top * 16u) & (vu->mem_size - 16u)), 16);
        vu_top_hist[hdr[0] & 0xFFFFu ? (hdr[0] & 0xFFFFu) < 64u
                                     ? (hdr[0] & 0xFFFFu) : 64u : 0u]++;
        if (ps2_diag_armed) {
            static u64 shown;
            if (shown++ < 16)
                ps2_log("vif1: activate  TOP=%u TOPS=%u BASE=%u OFST=%u DBF=%u "
                        "ITOP=%u | header %08X %08X %08X %08X  (word0 low half "
                        "%u -- NOT a vertex count, see ps2_vu.c)",
                        v->top, v->tops, v->base, v->offset, v->dbf, v->itop,
                        hdr[0], hdr[1], hdr[2], hdr[3], hdr[0] & 0xFFFFu);
        }
    }
}

static void vif_exec_code(ps2_vif *v, u32 code) {
    u32 irq = code >> 31;
    u32 cmd = (code >> 24) & 0x7Fu;
    {
        static int budget = -1;
        static u64 nseen[2];
        static int was_armed;
        if (budget < 0) {
            const char *e = getenv("PS2_TRACE_VIF");
            budget = e ? (int)strtol(e, NULL, 0) : 0;
        }
        if (budget > 0) {
            if (ps2_diag_armed && !was_armed) nseen[0] = nseen[1] = 0;
            was_armed = ps2_diag_armed;
            if (ps2_diag_armed
                && nseen[v->is_vif1 ? 1 : 0]++ < (u64)budget)
                ps2_log("vif%d: code %08X  cmd=%02X num=%u imm=%04X  %s%s",
                        v->is_vif1 ? 1 : 0, code, cmd,
                        (code >> 16) & 0xFFu, code & 0xFFFFu,
                        vif_cmd_name(cmd), (code >> 31) ? "  [i]" : "");
        }
    }
    u32 num = (code >> 16) & 0xFFu;
    u32 imm = code & 0xFFFFu;
    ps2_vu *vu = v->is_vif1 ? &vu1 : &ps2_cpu.vu0;

    vif_cmd_hist[v->is_vif1 ? 1 : 0][cmd]++;
    v->code = code;
    v->cmd = cmd;
    v->num = num;
    v->imm = imm;
    v->wait = 0;

    vif_ring_push(VIF_EV_CODE, v->is_vif1 ? 1 : 0, code, 0,
                  ((v->cycle_cl & 0xFFu) << 8) | (v->cycle_wl & 0xFFu));
    if (irq) {
        vif_irq_codes++;
        ps2_intc_raise(v->is_vif1 ? PS2_INTC_VIF1 : PS2_INTC_VIF0);
    }
    if (!vif_cmd_valid(cmd)) {
        vif_bad_cmd++;
        if (!v->err_stall) vif_stalls++;
        v->err_stall = 1;
        vif_desync_report("undefined VIFcode", v->is_vif1 ? 1 : 0, code);
        return;
    }

    if (cmd >= 0x60) {
        u32 elems = num ? num : 256u;
        u32 cl, wl, read_elems, ebits;
        v->unpack_vl = cmd & 3u;
        v->unpack_vn = (cmd >> 2) & 3u;
        v->unpack_m = (cmd >> 4) & 1u;
        v->unpack_usn = (imm >> 14) & 1u;
        v->unpack_base = (imm & 0x3FFu)
                       + ((v->is_vif1 && ((imm >> 15) & 1u)) ? v->tops : 0u);
        v->addr = v->unpack_base;
        v->unpack_num = elems;
        v->unpack_written = 0;
        v->unpack_index = 0;
        v->unpack_align = v->word_lane ? v->word_lane : 4u;
        v->bits = 0; v->nbits = 0; v->elem_n = 0;
        cl = v->cycle_cl ? v->cycle_cl : 256u;
        wl = v->cycle_wl ? v->cycle_wl : 256u;
        v->unpack_cl = cl;
        v->unpack_wl = wl;
        if (wl > cl) {
            u32 rem = elems % wl;
            read_elems = cl * (elems / wl) + (rem > cl ? cl : rem);
        } else {
            read_elems = elems;
        }
        ebits = vif_unpack_bits(v);
        v->wait = (read_elems * ebits + 31u) / 32u;
        if (v->is_vif1) vif_unpack_census(cmd, cl, wl, elems, v->unpack_m,
                                          (imm >> 15) & 1u, imm & 0x3FFu);
        if (!v->wait) {
            ps2_vu *tvu = v->is_vif1 ? &vu1 : &ps2_cpu.vu0;
            while (v->unpack_num && vif_unpack_is_fill(v)) vif_unpack_fill(v, tvu);
        }
        return;
    }
    switch (cmd) {
    case 0x00: break;
    case 0x01: v->cycle_cl = imm & 0xFFu; v->cycle_wl = (imm >> 8) & 0xFFu; break;
    case 0x02:
        v->offset = imm & 0x3FFu;
        v->dbf = 0;
        v->tops = v->base;
        break;
    case 0x03:
        v->base = imm & 0x3FFu;
        v->tops = v->base + (v->dbf ? v->offset : 0u);
        break;
    case 0x04: v->itops = imm & 0x3FFu; break;
    case 0x05: v->mode = imm & 3u; break;
    case 0x06: break;
    case 0x07: v->mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;
    case 0x14:
    case 0x15:
        if (imm * 8u >= vu->micro_size) {
            vif_bad_mscal++;
            vif_desync_report("MSCAL outside micro memory", v->is_vif1 ? 1 : 0,
                              code);
            break;
        }
        vif_activate(v, vu);
        vu_stat_mscal++;
        if (v->is_vif1) {
            vu_mscal_at[((imm * 8u) >> 10) & 63u]++;
            if (ps2_diag_armed) {
                static u64 shown;
                if (shown++ < 12)
                    ps2_log("vu1: MSCAL imm=%04X -> byte %04X (instruction %u)",
                            imm, imm * 8u, imm);
            }
        }
        if (vu1_ready || !v->is_vif1) vu_run(vu, imm * 8u);
        break;
    case 0x17:
        vif_activate(v, vu);
        vu_stat_mscal++;
        if (v->is_vif1) vu_mscnt_n++;
        if (vu1_ready || !v->is_vif1) vu_run(vu, vu->tpc);
        break;
    case 0x20: v->wait = 1; break;
    case 0x30: v->wait = 4; break;
    case 0x31: v->wait = 4; break;
    case 0x4A: {
        u32 insns = num ? num : 256u;
        u32 base = (imm & 0xFFFFu) * 8u;
        if (base + insns * 8u > vu->micro_size) {
            vif_bad_cmd++;
            if (!v->err_stall) vif_stalls++;
            v->err_stall = 1;
            vif_desync_report("MPG outside micro memory",
                              v->is_vif1 ? 1 : 0, code);
            break;
        }
        v->mpg_addr = base;
        v->wait = insns * 2u;
        break;
    }
    case 0x50:
    case 0x51:
        v->direct_left = imm ? imm : 65536u;
        v->wait = v->direct_left * 4u;
        break;
    default:
        break;
    }
}

static void vif_data_qw(ps2_vif *v, const ps2_reg128 *q) {
    ps2_vu *vu = v->is_vif1 ? &vu1 : &ps2_cpu.vu0;
    switch (v->cmd) {
    case 0x20: v->mask = q->uw[0]; break;
    case 0x30: memcpy(v->row, q->uw, 16); break;
    case 0x31: memcpy(v->col, q->uw, 16); break;
    case 0x4A: {
        u32 off = v->mpg_addr;
        if (off + 16u > vu->micro_size) break;
        vu_stat_mpg++;
        if (memcmp(vu->micro + off, q, 16) != 0) {
            memcpy(vu->micro + off, q, 16);
            vu_prog_dirty = 1;
        }
        if (v->is_vif1) {
            vu_mpg_at[(off >> 10) & 63u]++;
            if (off < vu_mpg_lo) vu_mpg_lo = off;
            if (off + 16u > vu_mpg_hi) vu_mpg_hi = off + 16u;
        }
        v->mpg_addr += 16;
        break;
    }
    case 0x50:
    case 0x51:
        ps2_gif_path = 2;
        ps2_gif_transfer(q, 1);
        ps2_gif_path = 0;
        break;
    default:
        break;
    }
}

static void vif_word(ps2_vif *v, u32 w) {
    v->word_lane = (v->word_lane + 1u) & 3u;
    if (!v->wait && v->unpack_num && v->elem_n == 3u) {
        v->bits = w; v->nbits = 32;
        vif_unpack_drain(v, v->is_vif1 ? &vu1 : &ps2_cpu.vu0);
        v->bits = 0; v->nbits = 0;
    }
    if (v->err_stall) return;
    if (!v->wait) {
        vif_exec_code(v, w);
        vif_ring_setwait(v->wait);
        v->accn = 0;
        return;
    }
    v->wait--;
    if (v->cmd >= 0x60) {
        vu_stat_unpack++;
        vif_unpack_word(v, v->is_vif1 ? &vu1 : &ps2_cpu.vu0, w);
        return;
    }
    v->acc[v->accn++] = w;
    if (v->accn == 4 || v->wait == 0) {
        ps2_reg128 q;
        while (v->accn < 4) v->acc[v->accn++] = 0;
        memcpy(&q, v->acc, 16);
        v->accn = 0;
        vif_data_qw(v, &q);
    }
}

static void vif_quadword(ps2_vif *v, const u32 *words) {
    u32 lane = 0;
    v->word_lane = 0;
    while (lane < 4u) {
        if (v->wait && v->cmd >= 0x60u && !v->err_stall) {
            u32 avail = 4u - lane;
            u32 n = v->wait < avail ? v->wait : avail;
            ps2_vu *vu = v->is_vif1 ? &vu1 : &ps2_cpu.vu0;
            u32 k;
            PS2_PHASE_BEGIN(PS2_PH_UNPACK);
            for (k = 0; k < n; k++) {
                v->wait--;
                vif_unpack_stream(v, vu, words[lane + k]);
            }
            PS2_PHASE_END(PS2_PH_UNPACK);
            vu_stat_unpack += n;
            lane += n;
            v->word_lane = lane & 3u;
            continue;
        }
        vif_word(v, words[lane]);
        lane++;
    }
}

void ps2_vif_fifo(int which, const ps2_reg128 *q) {
    ps2_vif *v = &vif[which & 1];
    PS2_PHASE_BEGIN(PS2_PH_VIF);
    vif_quadword(v, q->uw);
    PS2_PHASE_END(PS2_PH_VIF);
}

void ps2_vif_transfer(int which, const ps2_reg128 *q, u32 qwc) {
    ps2_vif *v = &vif[which & 1];
    const u8 *src = (const u8 *)q;
    PS2_PHASE_BEGIN(PS2_PH_VIF);
    for (u32 i = 0; i < qwc && !v->err_stall;) {
        if ((v->cmd == 0x50u || v->cmd == 0x51u) && v->wait >= 4u
            && !v->accn && !ps2_diag_armed && !g_cap_shallow && !g_cap_deep) {
            u32 n = v->wait / 4u;
            if (n > qwc - i) n = qwc - i;
            v->wait -= n * 4u;
            v->word_lane = 0;
            memcpy(v->acc, src + (size_t)(i + n - 1u) * 16u, 16);
            ps2_gif_path = 2;
            ps2_gif_transfer((const ps2_reg128 *)(src + (size_t)i * 16u), n);
            ps2_gif_path = 0;
            i += n;
            continue;
        }
        u32 words[4];
        memcpy(words, src + (size_t)i * 16u, sizeof(words));
        vif_quadword(v, words);
        i++;
    }
    PS2_PHASE_END(PS2_PH_VIF);
}

int ps2_vif_stalled(int which) { return vif[which & 1].err_stall != 0; }

void ps2_vu_state_save(ps2_state_put put, void *ud) {
    put(ud, "vu1_micro", vu1.micro, vu1.micro ? vu1.micro_size : 0);
    put(ud, "vu1_mem",   vu1.mem,   vu1.mem   ? vu1.mem_size   : 0);
    put(ud, "vu1_regs",  &vu1, sizeof vu1);
    put(ud, "vu0_micro", ps2_cpu.vu0.micro,
        ps2_cpu.vu0.micro ? ps2_cpu.vu0.micro_size : 0);
    put(ud, "vu0_mem",   ps2_cpu.vu0.mem,
        ps2_cpu.vu0.mem   ? ps2_cpu.vu0.mem_size   : 0);
    put(ud, "vu0_regs",  &ps2_cpu.vu0, sizeof ps2_cpu.vu0);
    put(ud, "vif0",      &vif[0], sizeof vif[0]);
    put(ud, "vif1",      &vif[1], sizeof vif[1]);
    put(ud, "vif_ring",  vif_ring, sizeof vif_ring);
    put(ud, "vif_ring_seq", &vif_ring_seq, sizeof vif_ring_seq);
}

_Static_assert(sizeof(ps2_vif) <= PS2_CAP_VIF_BLOB,
               "ps2_vif has outgrown the capture blob; raise PS2_CAP_VIF_BLOB");

void ps2_vu_cap_save(ps2_vu_capstate *st) {
    memset(st, 0, sizeof *st);
    st->vu1 = vu1;
    st->vu0 = ps2_cpu.vu0;
    st->vu1.mem = st->vu1.micro = NULL;
    st->vu0.mem = st->vu0.micro = NULL;
    st->vif_size = (u32)sizeof(ps2_vif);
    memcpy(st->vif[0], &vif[0], sizeof(ps2_vif));
    memcpy(st->vif[1], &vif[1], sizeof(ps2_vif));
}

void ps2_vu_cap_load(const ps2_vu_capstate *st) {
    u8 *mem, *micro;
    u32 msz, usz;
    if (st->vif_size != (u32)sizeof(ps2_vif) && st->vif_size != ((offsetof(ps2_vif, word_lane) + 7u) & ~7u)) {
        ps2_log("cap: VIF state is %u bytes in the capture and %u in this "
                "build -- VIF latches not restored", st->vif_size,
                (u32)sizeof(ps2_vif));
    } else {
        memset(vif, 0, sizeof(vif));
        memcpy(&vif[0], st->vif[0], st->vif_size);
        memcpy(&vif[1], st->vif[1], st->vif_size);
        if (st->vif_size != sizeof(ps2_vif)) {
            for (unsigned i=0;i<2;i++) vif[i].word_lane=vif[i].unpack_align=vif[i].unpack_index=0;
            ps2_log("cap: legacy VIF snapshot lacks source alignment; recapture for exact replay");
        }
        vif[1].is_vif1 = 1;
    }
    mem = vu1.mem; micro = vu1.micro; msz = vu1.mem_size; usz = vu1.micro_size;
    vu1 = st->vu1;
    vu1.mem = mem; vu1.micro = micro;
    vu1.mem_size = msz; vu1.micro_size = usz;
    mem = ps2_cpu.vu0.mem; micro = ps2_cpu.vu0.micro;
    msz = ps2_cpu.vu0.mem_size; usz = ps2_cpu.vu0.micro_size;
    ps2_cpu.vu0 = st->vu0;
    ps2_cpu.vu0.mem = mem; ps2_cpu.vu0.micro = micro;
    ps2_cpu.vu0.mem_size = msz; ps2_cpu.vu0.micro_size = usz;
}

u8 *ps2_vu_cap_mem(int which, u32 *size) {
    ps2_vu *v = which ? &vu1 : &ps2_cpu.vu0;
    if (size) *size = v->mem ? v->mem_size : 0u;
    return v->mem;
}

u8 *ps2_vu_cap_micro(int which, u32 *size) {
    ps2_vu *v = which ? &vu1 : &ps2_cpu.vu0;
    if (size) *size = v->micro ? v->micro_size : 0u;
    return v->micro;
}

void ps2_vif_clear_stall(int which) {
    ps2_vif *v = &vif[which & 1];
    if (!v->err_stall) return;
    v->err_stall = 0;
    v->wait = 0;
    v->accn = 0;
    v->direct_left = 0;
    v->unpack_num = 0;
    v->bits = 0;
    v->nbits = 0;
    v->elem_n = 0;
}

void ps2_vif_fbrst(int which, u32 val) {
    ps2_vif *v = &vif[which & 1];
    if (val & 1u) {
        v->err_stall = 0;
        v->wait = 0; v->accn = 0; v->direct_left = 0; v->unpack_num = 0;
        v->bits = 0; v->nbits = 0; v->elem_n = 0;
    }
    if (val & 8u) ps2_vif_clear_stall(which);
}

void ps2_vif_write(int which, u32 word) {
    PS2_PHASE_BEGIN(PS2_PH_VIF);
    if (which & 0x80) vif[which & 1].word_lane = ((u32)which >> 1) & 3u;
    vif_word(&vif[which & 1], word);
    PS2_PHASE_END(PS2_PH_VIF);
}

void ps2_vu_stats(u32 *unknown_lower_ops) { *unknown_lower_ops = vu_unknown_n; }

void ps2_vu_perf(u64 *insns, u64 *runs) {
    if (insns) *insns = vu_stat_insns;
    if (runs) *runs = vu_stat_mscal + vu_mscnt_n;
}

void ps2_vif_hist_report(void) {
    for (int w = 0; w < 2; w++) {
        char line[512];
        int n = 0;
        for (int c = 0; c < 256; c++) {
            if (!vif_cmd_hist[w][c]) continue;
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X:%llu",
                          c, (unsigned long long)vif_cmd_hist[w][c]);
            if (n > 400) break;
        }
        if (n) ps2_log("vif%d codes:%s", w, line);
    }
    ps2_vif_sync_report();
}

void ps2_vu_census_dump(void) {
    char path[512];
    FILE *mf;
    u32 i, j;
    if (!vu_census_enabled() || !vu_prog_n) return;
    ps2_mkdir_p("out/vu_programs");
    snprintf(path, sizeof path, "out/vu_programs/manifest.json");
    mf = fopen(path, "wb");
    if (!mf) { ps2_log("vu1: cannot write %s", path); return; }
    fprintf(mf, "{\n  \"programs\": [\n");
    for (i = 0; i < vu_prog_n; i++) {
        vu_prog *p = &vu_progs[i];
        FILE *bf;
        snprintf(path, sizeof path, "out/vu_programs/vu1_%016llx.bin",
                 (unsigned long long)p->crc);
        bf = fopen(path, "wb");
        if (bf) { fwrite(p->image, 1, p->image_size, bf); fclose(bf); }
        fprintf(mf, "    {\n      \"crc\": \"%016llx\",\n"
                    "      \"image\": \"vu1_%016llx.bin\",\n"
                    "      \"bytes\": %u,\n"
                    "      \"code_lo\": %u,\n      \"code_hi\": %u,\n"
                    "      \"activations\": %llu,\n"
                    "      \"instructions\": %llu,\n"
                    "      \"xgkicks\": %llu,\n"
                    "      \"entries\": [",
                (unsigned long long)p->crc, (unsigned long long)p->crc,
                p->image_size, p->lo, p->hi,
                (unsigned long long)p->activations,
                (unsigned long long)p->insns,
                (unsigned long long)p->kicks);
        for (j = 0; j < p->n_entries; j++)
            fprintf(mf, "%s\n        {\"pc\": %u, \"count\": %llu}",
                    j ? "," : "", p->entries[j],
                    (unsigned long long)p->entry_n[j]);
        fprintf(mf, "\n      ]\n    }%s\n", i + 1 < vu_prog_n ? "," : "");
    }
    fprintf(mf, "  ],\n  \"dropped\": %llu\n}\n",
            (unsigned long long)vu_prog_dropped);
    fclose(mf);
    ps2_log("vu1: wrote out/vu_programs/manifest.json (%u images)", vu_prog_n);
}

void ps2_vu_census_report(void) {
    u32 i, j;
    if (!vu_census_enabled()) return;
    if (!vu_prog_n) { ps2_log("vu1 census: no microprogram ever ran"); return; }
    ps2_log("vu1 census: %u distinct microcode images%s", vu_prog_n,
            vu_prog_dropped ? "  (TABLE FULL -- some were not recorded)" : "");
    for (i = 0; i < vu_prog_n; i++) {
        vu_prog *p = &vu_progs[i];
        char line[400];
        int n = 0;
        for (j = 0; j < p->n_entries && n < 300; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %04X(x%llu)",
                          p->entries[j], (unsigned long long)p->entry_n[j]);
        line[n] = 0;
        ps2_log("  %016llx  code %04X..%04X (%u pairs)  %llu runs, "
                "%llu insns, %llu kicks", (unsigned long long)p->crc,
                p->lo, p->hi, (p->hi - p->lo) / 8u,
                (unsigned long long)p->activations,
                (unsigned long long)p->insns,
                (unsigned long long)p->kicks);
        ps2_log("     entries:%s%s", line,
                p->n_entries >= 32 ? "  (32 shown, list full)" : "");
    }
    ps2_vu_census_dump();
}

void ps2_vu_micro_report(void) {
    unsigned i;
    u32 a;
    unsigned progs = 0, xgk = 0;
    ps2_vu_recomp_report();
    ps2_vu_census_report();
    if (vu_mpg_lo == 0xFFFFFFFFu) { ps2_log("vu1: no microcode uploaded"); return; }
    ps2_log("vu1: microcode occupies %04X..%04X (instructions %u..%u)",
            vu_mpg_lo, vu_mpg_hi, vu_mpg_lo / 8u, vu_mpg_hi / 8u);
    ps2_log("vu1: MPG quadwords by 1 KB bucket:");
    for (i = 0; i < 64; i++)
        if (vu_mpg_at[i])
            ps2_log("   %04X..%04X  x%llu", i * 1024u, i * 1024u + 1023u,
                    (unsigned long long)vu_mpg_at[i]);
    ps2_log("vu1: MSCAL start addresses by 1 KB bucket (MSCNT x%llu, all at 0):",
            (unsigned long long)vu_mscnt_n);
    for (i = 0; i < 64; i++)
        if (vu_mscal_at[i])
            ps2_log("   %04X..%04X  x%llu", i * 1024u, i * 1024u + 1023u,
                    (unsigned long long)vu_mscal_at[i]);
    for (a = 0; a + 8u <= vu1.micro_size; a += 8u) {
        u32 lo, up;
        memcpy(&lo, vu1.micro + a, 4);
        memcpy(&up, vu1.micro + a + 4, 4);
        if (up & 0x40000000u) progs++;
        if (!(up & 0x80000000u) && ((lo >> 25) & 0x7Fu) == 0x40
            && (lo & 0x7FFu) == 0x6FCu) {
            if (xgk < 8)
                ps2_log("   XGKICK resident at %04X (instruction %u), Is=VI%02u",
                        a, a / 8u, (lo >> 11) & 0x1Fu);
            xgk++;
        }
    }
    ps2_log("vu1: %llu instructions executed, %llu runs hit the guard%s",
            (unsigned long long)vu_stat_insns,
            (unsigned long long)vu_stat_guard_hits,
            vu_stat_guard_hits ? "  (each reset the continue-PC to 0, so the "
                                 "next activation re-enters the program)" : "");
    {
        u64 tot = 0, zero = vu_top_hist[0];
        char line[512];
        int n = 0;
        unsigned i;
        for (i = 0; i < 66; i++) tot += vu_top_hist[i];
        for (i = 1; i < 66 && n < 400; i++)
            if (vu_top_hist[i])
                n += snprintf(line + n, sizeof line - (size_t)n, " %u:%llu",
                              i, (unsigned long long)vu_top_hist[i]);
        line[n] = 0;
        ps2_log("vu1: vertex counts at TOP -- %llu activations, %llu with a "
                "count of ZERO%s", (unsigned long long)tot,
                (unsigned long long)zero, tot && zero == tot
                ? "  <-- the double buffer is not advancing" : "");
        if (n) ps2_log("   counts (64 = 64 or more):%s", line);
    }
    {   char line[512];
        int n = 0;
        unsigned i;
        for (i = 0; i < 24 && n < 400; i++)
            if (vu_runlen_hist[i])
                n += snprintf(line + n, sizeof line - (size_t)n, " %u:%llu",
                              1u << i, (unsigned long long)vu_runlen_hist[i]);
        line[n] = 0;
        ps2_log("vu1: run lengths by power of two:%s", line);
        ps2_log("vu1: %llu of %llu runs reached an XGKICK",
                (unsigned long long)vu_runs_with_kick,
                (unsigned long long)vu_stat_mscal);
    }
    ps2_vif_unpack_report();
    ps2_log("vu1: XGKICK moved %llu quadwords in %llu kicks (%.1f qw each)",
            (unsigned long long)vu_stat_xgkick_qw,
            (unsigned long long)vu_stat_xgkick,
            vu_stat_xgkick ? (double)vu_stat_xgkick_qw / (double)vu_stat_xgkick
                           : 0.0);
    ps2_log("vu1: %u [E] markers and %u XGKICKs resident in micro memory",
            progs, xgk);
    {
        u32 k;
        for (k = 1370; k < 1392 && k * 8u + 8u <= vu1.micro_size; k++) {
            u32 lo, up;
            memcpy(&lo, vu1.micro + k * 8u, 4);
            memcpy(&up, vu1.micro + k * 8u + 4, 4);
            ps2_log("   tail[%u] lo=%08X up=%08X  lo11=%03X%s%s", k, lo, up,
                    lo & 0x7FFu, (up & 0x40000000u) ? "  [E]" : "",
                    (up & 0x80000000u) ? "  [I]" : "");
        }
    }
    {
        u32 k;
        for (k = 0; k < 6 && k * 8u + 8u <= vu1.micro_size; k++) {
            u32 lo, up;
            memcpy(&lo, vu1.micro + k * 8u, 4);
            memcpy(&up, vu1.micro + k * 8u + 4, 4);
            ps2_log("   micro[%u] lo=%08X up=%08X%s", k, lo, up,
                    (up & 0x40000000u) ? "  [E]" : "");
        }
    }
}

void ps2_vu_dump(void) {
    FILE *fp;
    if (!PS2_ENV("PS2_DUMP_VU1")) return;
    fp = fopen("vu1_micro.bin", "wb");
    if (fp) { fwrite(vu1.micro, 1, vu1.micro_size, fp); fclose(fp);
              ps2_log("vu1: micro memory written to vu1_micro.bin (%u bytes)",
                      vu1.micro_size); }
    fp = fopen("vu1_data.bin", "wb");
    if (fp) { fwrite(vu1.mem, 1, vu1.mem_size, fp); fclose(fp);
              ps2_log("vu1: data memory written to vu1_data.bin (%u bytes)",
                      vu1.mem_size); }
}

static const char *vu_up_name(u32 k) {
    static const char *bc = "xyzw";
    static char buf[24];
    static const char *base[8] = { "ADD", "SUB", "MADD", "MSUB",
                                   "MAX", "MINI", "MUL", "?" };
    if (k < 0x80u) {
        if (k < 0x1Cu) {
            snprintf(buf, sizeof buf, "%s%c", base[k >> 2], bc[k & 3]);
            return buf;
        }
        switch (k) {
        case 0x1C: return "MULq";   case 0x1D: return "MAXi";
        case 0x1E: return "MULi";   case 0x1F: return "MINIi";
        case 0x20: return "ADDq";   case 0x21: return "MADDq";
        case 0x22: return "ADDi";   case 0x23: return "MADDi";
        case 0x24: return "SUBq";   case 0x25: return "MSUBq";
        case 0x26: return "SUBi";   case 0x27: return "MSUBi";
        case 0x28: return "ADD";    case 0x29: return "MADD";
        case 0x2A: return "MUL";    case 0x2B: return "MAX";
        case 0x2C: return "SUB";    case 0x2D: return "MSUB";
        case 0x2E: return "OPMSUB"; case 0x2F: return "MINI";
        default: break;
        }
    } else {
        u32 sub = k & 0x7Fu;
        if (sub < 0x10u) {
            static const char *a[4] = { "ADDA", "SUBA", "MADDA", "MSUBA" };
            snprintf(buf, sizeof buf, "%s%c", a[sub >> 2], bc[sub & 3]);
            return buf;
        }
        switch (sub) {
        case 0x1D: return "ABS";    case 0x1F: return "CLIP";
        case 0x2A: return "MULA";   case 0x2E: return "OPMULA";
        case 0x2F: return "NOP";
        default: break;
        }
        if (sub >= 0x10u && sub <= 0x13u) return "ITOF";
        if (sub >= 0x14u && sub <= 0x17u) return "FTOI";
        if (sub >= 0x18u && sub <= 0x1Bu) return "MULAbc";
    }
    snprintf(buf, sizeof buf, "?%02X", k);
    return buf;
}

static const char *vu_lo_name(u32 sub) {
    switch (sub) {
    case 0x33C: return "MOVE/NOP"; case 0x33D: return "MR32";
    case 0x37C: return "LQI";      case 0x37D: return "SQI";
    case 0x37E: return "LQD";      case 0x37F: return "SQD";
    case 0x3BC: return "DIV";      case 0x3BD: return "SQRT";
    case 0x3BE: return "RSQRT";    case 0x3BF: return "WAITQ";
    case 0x3FC: return "MTIR";     case 0x3FD: return "MFIR";
    case 0x3FE: return "ILWR";     case 0x3FF: return "ISWR";
    case 0x43C: return "RNEXT";    case 0x43D: return "RGET";
    case 0x43E: return "RINIT";    case 0x43F: return "RXOR";
    case 0x67C: return "MFP";      case 0x6BC: return "XTOP";
    case 0x6BD: return "XITOP";    case 0x6FC: return "XGKICK";
    default: break;
    }
    if ((sub & 0x3Fu) == 0x30u) return "IADD";
    if ((sub & 0x3Fu) == 0x31u) return "ISUB";
    if ((sub & 0x3Fu) == 0x32u) return "IADDI";
    if ((sub & 0x3Fu) == 0x34u) return "IAND";
    if ((sub & 0x3Fu) == 0x35u) return "IOR";
    return "?";
}

void ps2_vu_profile_report(void) {
    u64 tot = 0, ptot = 0;
    unsigned i, shown;
    if (vu_profile != 1) return;
    for (i = 0; i < VU_PROF_SLOTS; i++) tot += vu_pc_hist[i];
    if (!tot) { ps2_log("vu1 profile: nothing executed"); return; }
    for (i = 0; i < VU_PROF_SLOTS; i++) ptot += vu_prog_insns[i];

    ps2_log("---- VU1 execution profile (%llu instructions) ----",
            (unsigned long long)tot);
    ps2_log("  by microprogram (MSCAL entry point), instructions first:");
    for (shown = 0; shown < 12; shown++) {
        unsigned best = 0; u64 bv = 0;
        for (i = 0; i < VU_PROF_SLOTS; i++)
            if (vu_prog_insns[i] > bv) { bv = vu_prog_insns[i]; best = i; }
        if (!bv) break;
        ps2_log("    %04X  %6.2f%%  %12llu insns  %8llu runs  %6.0f insns/run",
                best * 8u, 100.0 * (double)bv / (double)ptot,
                (unsigned long long)bv,
                (unsigned long long)vu_prog_runs[best],
                (double)bv / (double)(vu_prog_runs[best] ? vu_prog_runs[best] : 1));
        vu_prog_insns[best] = 0;
    }
    ps2_log("  hottest instruction slots:");
    for (shown = 0; shown < 16; shown++) {
        unsigned best = 0; u64 bv = 0;
        for (i = 0; i < VU_PROF_SLOTS; i++)
            if (vu_pc_hist[i] > bv) { bv = vu_pc_hist[i]; best = i; }
        if (!bv) break;
        ps2_log("    %04X  %5.2f%%  x%llu", best * 8u,
                100.0 * (double)bv / (double)tot, (unsigned long long)bv);
        vu_pc_hist[best] = 0;
    }
    ps2_log("  upper opcodes as executed:");
    for (shown = 0; shown < 12; shown++) {
        unsigned best = 0; u64 bv = 0;
        for (i = 0; i < 256; i++)
            if (vu_up_hist[i] > bv) { bv = vu_up_hist[i]; best = i; }
        if (!bv) break;
        ps2_log("    %-10s %5.2f%%  x%llu", vu_up_name(best),
                100.0 * (double)bv / (double)tot, (unsigned long long)bv);
        vu_up_hist[best] = 0;
    }
    ps2_log("  lower special-group opcodes as executed:");
    for (shown = 0; shown < 12; shown++) {
        unsigned best = 0; u64 bv = 0;
        for (i = 0; i < 2048; i++)
            if (vu_lo_sub_hist[i] > bv) { bv = vu_lo_sub_hist[i]; best = i; }
        if (!bv) break;
        ps2_log("    %-10s %5.2f%%  x%llu  (%03X)", vu_lo_name(best),
                100.0 * (double)bv / (double)tot,
                (unsigned long long)bv, best);
        vu_lo_sub_hist[best] = 0;
    }
}

void ps2_vu_lower_hist_report(void) {
    unsigned i, j;
    u64 tot = 0;
    for (i = 0; i < 128; i++) tot += vu_low_hist[i];
    if (!tot) return;
    ps2_log("VU lower primary opcodes executed (%llu total):",
            (unsigned long long)tot);
    for (i = 0; i < 128; i++)
        if (vu_low_hist[i])
            ps2_log("   op %02X  x%llu", i, (unsigned long long)vu_low_hist[i]);
    ps2_log("VU lower special group (op 0x40), by (bits 5:0, bits 10:6):");
    for (i = 0; i < 64; i++)
        for (j = 0; j < 32; j++)
            if (vu_sp_pair[i][j])
                ps2_log("   lo6=%02X hi5=%02X  x%llu  [sub as decoded now=%02X]",
                        i, j, (unsigned long long)vu_sp_pair[i][j],
                        (unsigned)((i & 3u) | ((((i | (j << 6)) >> 4) & 0x7Cu))));
}

void ps2_vu_counters(u64 *mpg, u64 *mscal, u64 *xgkick, u64 *unpack) {
    if (mpg) *mpg = vu_stat_mpg;
    if (mscal) *mscal = vu_stat_mscal;
    if (xgkick) *xgkick = vu_stat_xgkick;
    if (unpack) *unpack = vu_stat_unpack;
}

static u32 vif_test_fail;

static void vif_test_expect(const char *name, unsigned qw,
                            u32 x, u32 y, u32 z, u32 w) {
    u32 got[4];
    memcpy(got, vu1.mem + qw * 16u, 16);
    if (got[0] == x && got[1] == y && got[2] == z && got[3] == w) return;
    vif_test_fail++;
    ps2_log("  FAIL %-28s qw%-3u  got %08X %08X %08X %08X",
            name, qw, got[0], got[1], got[2], got[3]);
    ps2_log("       %-28s        want %08X %08X %08X %08X", "", x, y, z, w);
}

static void vif_test_run(u32 cmd, u32 num, u32 imm,
                         const u32 *payload, unsigned nwords) {
    unsigned i;
    memset(vu1.mem, 0xCD, vu1.mem_size);
    vif[1].wait = 0;
    vif[1].accn = 0;
    vif[1].word_lane = 3;
    ps2_vif_write(1, (cmd << 24) | ((num & 0xFFu) << 16) | (imm & 0xFFFFu));
    for (i = 0; i < nwords; i++) ps2_vif_write(1, payload[i]);
    ps2_vif_write(1, 0);
}

static void vif_test_cycle(u32 cl, u32 wl) {
    ps2_vif_write(1, (0x01u << 24) | ((wl & 0xFFu) << 8) | (cl & 0xFFu));
}

int ps2_vif_selftest(void) {
    u32 saved_mask = vif[1].mask, saved_mode = vif[1].mode;
    u32 saved_row[4], saved_col[4];
    memcpy(saved_row, vif[1].row, 16);
    memcpy(saved_col, vif[1].col, 16);
    vif_test_fail = 0;
    if (!vu1.mem) { ps2_log("vif selftest: VU1 not initialised"); return 1; }

    ps2_log("---- VIF UNPACK self-test (EE_Users_Manual.pdf 6.3.4-6.3.6) ----");

    vif[1].mode = 0;
    vif[1].mask = 0;
    vif[1].row[0] = 0x11111111u; vif[1].row[1] = 0x22222222u;
    vif[1].row[2] = 0x33333333u; vif[1].row[3] = 0x44444444u;
    vif[1].col[0] = 0xC0C0C0C0u; vif[1].col[1] = 0xC1C1C1C1u;
    vif[1].col[2] = 0xC2C2C2C2u; vif[1].col[3] = 0xC3C3C3C3u;
    vif_test_cycle(1, 1);

    {
        static const u32 pl[3] = {0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u};
        vif_test_run(0x60, 3, 0, pl, 3);
        vif_test_expect("S-32 broadcast", 0, 0xAAAA0001u, 0xAAAA0001u,
                        0xAAAA0001u, 0xAAAA0001u);
        vif_test_expect("S-32 broadcast", 2, 0xAAAA0003u, 0xAAAA0003u,
                        0xAAAA0003u, 0xAAAA0003u);
    }
    {
        static const u32 pl[2] = {0x8000FFFFu, 0x00007FFFu};
        vif_test_run(0x61, 3, 0, pl, 2);
        vif_test_expect("S-16 signed", 0, 0xFFFFFFFFu, 0xFFFFFFFFu,
                        0xFFFFFFFFu, 0xFFFFFFFFu);
        vif_test_expect("S-16 signed", 1, 0xFFFF8000u, 0xFFFF8000u,
                        0xFFFF8000u, 0xFFFF8000u);
        vif_test_expect("S-16 signed", 2, 0x00007FFFu, 0x00007FFFu,
                        0x00007FFFu, 0x00007FFFu);
    }
    {
        static const u32 pl[1] = {0x000080FFu};
        vif_test_run(0x62, 2, 1u << 14, pl, 1);
        vif_test_expect("S-8 unsigned", 0, 0xFFu, 0xFFu, 0xFFu, 0xFFu);
        vif_test_expect("S-8 unsigned", 1, 0x80u, 0x80u, 0x80u, 0x80u);
    }
    {
        static const u32 pl[6] = {0x1000u, 0x2000u, 0x1001u, 0x2001u,
                                  0x1002u, 0x2002u};
        vif_test_run(0x64, 3, 0, pl, 6);
        vif_test_expect("V2-32 elem 0", 0, 0x1000u, 0x2000u,
                        0x1000u, 0x2000u);
        vif_test_expect("V2-32 elem 1", 1, 0x1001u, 0x2001u,
                        0x1001u, 0x2001u);
        vif_test_expect("V2-32 elem 2", 2, 0x1002u, 0x2002u,
                        0x1002u, 0x2002u);
    }
    {
        static const u32 pl[6] = {1, 2, 3, 4, 5, 6};
        vif_test_run(0x68, 2, 0, pl, 6);
        vif_test_expect("V3-32 elem 0", 0, 1, 2, 3, 4);
        vif_test_expect("V3-32 elem 1", 1, 4, 5, 6, 0);
    }
    {
        static const u32 pl[5] = {0x00020001u, 0x00040003u, 0x00060005u,
                                  0x00080007u, 0x00000009u};
        vif_test_run(0x69, 3, 1u << 14, pl, 5);
        vif_test_expect("V3-16 elem 0", 0, 1, 2, 3, 4);
        vif_test_expect("V3-16 elem 1", 1, 4, 5, 6, 7);
        vif_test_expect("V3-16 elem 2", 2, 7, 8, 9, 0);
    }
    {
        static const u32 pl[3] = {0x04030201u, 0x08070605u, 0x0C0B0A09u};
        vif_test_run(0x6E, 3, 1u << 14, pl, 3);
        vif_test_expect("V4-8 elem 0", 0, 1, 2, 3, 4);
        vif_test_expect("V4-8 elem 1", 1, 5, 6, 7, 8);
        vif_test_expect("V4-8 elem 2", 2, 9, 10, 11, 12);
    }
    {
        static const u32 pl[1] = {0x0000FFFFu};
        vif_test_run(0x6F, 1, 0, pl, 1);
        vif_test_expect("V4-5 all ones", 0, 0x1Fu << 3, 0x1Fu << 3,
                        0x1Fu << 3, 1u << 7);
    }
    {
        static const u32 pl[3] = {0xE0u, 0xE1u, 0xE2u};
        vif_test_cycle(2, 1);
        vif_test_run(0x60, 3, 0, pl, 3);
        vif_test_expect("skip CL2 WL1 n0", 0, 0xE0u, 0xE0u, 0xE0u, 0xE0u);
        vif_test_expect("skip CL2 WL1 n1", 2, 0xE1u, 0xE1u, 0xE1u, 0xE1u);
        vif_test_expect("skip CL2 WL1 n2", 4, 0xE2u, 0xE2u, 0xE2u, 0xE2u);
        vif_test_expect("skip CL2 WL1 gap", 1, 0xCDCDCDCDu, 0xCDCDCDCDu,
                        0xCDCDCDCDu, 0xCDCDCDCDu);
        vif_test_cycle(1, 1);
    }
    {
        static const u32 pl[4] = {0xF0u, 0xF1u, 0xF2u, 0xF3u};
        vif_test_cycle(4, 2);
        vif_test_run(0x60, 4, 0, pl, 4);
        vif_test_expect("skip CL4 WL2 n0", 0, 0xF0u, 0xF0u, 0xF0u, 0xF0u);
        vif_test_expect("skip CL4 WL2 n1", 1, 0xF1u, 0xF1u, 0xF1u, 0xF1u);
        vif_test_expect("skip CL4 WL2 n2", 4, 0xF2u, 0xF2u, 0xF2u, 0xF2u);
        vif_test_expect("skip CL4 WL2 n3", 5, 0xF3u, 0xF3u, 0xF3u, 0xF3u);
        vif_test_cycle(1, 1);
    }
    {
        static const u32 pl[2] = {0xB0u, 0xB1u};
        vif[1].mask = 0x00005500u;
        vif_test_cycle(1, 2);
        vif_test_run(0x70, 4, 0, pl, 2);
        vif_test_expect("fill CL1 WL2 read0", 0, 0xB0u, 0xB0u, 0xB0u, 0xB0u);
        vif_test_expect("fill CL1 WL2 fill0", 1, 0x11111111u, 0x22222222u,
                        0x33333333u, 0x44444444u);
        vif_test_expect("fill CL1 WL2 read1", 2, 0xB1u, 0xB1u, 0xB1u, 0xB1u);
        vif_test_expect("fill CL1 WL2 fill1", 3, 0x11111111u, 0x22222222u,
                        0x33333333u, 0x44444444u);
        vif[1].mask = 0;
        vif_test_cycle(1, 1);
    }
    {
        static const u32 pl[4] = {0xA0u, 0xA1u, 0xA2u, 0xA3u};
        vif[1].mask = (0u << 0) | (1u << 2) | (2u << 4) | (3u << 6);
        vif_test_run(0x7C, 1, 0, pl, 4);
        vif_test_expect("mask data/ROW/COL/skip", 0,
                        0xA0u, 0x22222222u, 0xC0C0C0C0u, 0xCDCDCDCDu);
        vif[1].mask = 0;
    }
    {
        static const u32 pl[4] = {1, 1, 1, 1};
        vif[1].mode = 1;
        vif_test_run(0x6C, 1, 0, pl, 4);
        vif_test_expect("MODE1 offset", 0, 0x11111112u, 0x22222223u,
                        0x33333334u, 0x44444445u);
        if (vif[1].row[0] != 0x11111111u) {
            vif_test_fail++;
            ps2_log("  FAIL MODE1 must not update ROW (row0 = %08X)",
                    vif[1].row[0]);
        }
        vif[1].mode = 2;
        vif_test_run(0x6C, 1, 0, pl, 4);
        if (vif[1].row[0] != 0x11111112u) {
            vif_test_fail++;
            ps2_log("  FAIL MODE2 must accumulate into ROW (row0 = %08X, "
                    "want 11111112)", vif[1].row[0]);
        }
        vif[1].mode = 0;
        vif[1].row[0] = 0x11111111u;
    }
    {
        static const u32 pl[2] = {
            0xFFFEFFFFu, 0x00008000u
        };
        vif_test_run(0x69, 1, 0, pl, 2);
        vif_test_expect("V3-16 signed sign-extends", 0,
                        0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFF8000u, 0);
        vif_test_run(0x69, 1, 1u << 14, pl, 2);
        vif_test_expect("V3-16 unsigned zero-extends", 0,
                        0x0000FFFFu, 0x0000FFFEu, 0x00008000u, 0);
    }
    {
        static const u32 pl[1] = {0x7F80FEFFu};
        vif_test_run(0x6E, 1, 0, pl, 1);
        vif_test_expect("V4-8 signed sign-extends", 0,
                        0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFF80u, 0x0000007Fu);
        vif_test_run(0x6E, 1, 1u << 14, pl, 1);
        vif_test_expect("V4-8 unsigned zero-extends", 0,
                        0x000000FFu, 0x000000FEu, 0x00000080u, 0x0000007Fu);
    }
    {
        static const u32 pl[2] = {0x00020001u, 0x8000FFFFu};
        vif_test_run(0x6D, 1, 0, pl, 2);
        vif_test_expect("V4-16 signed", 0,
                        1u, 2u, 0xFFFFFFFFu, 0xFFFF8000u);
        vif_test_run(0x65, 1, 0, pl, 1);
        vif_test_expect("V2-16 duplicates X and Y", 0,
                        1u, 2u, 1u, 2u);
    }
    {
        static const u32 pl[2] = {0x04030201u, 0x08070605u};
        vif_test_run(0x6A, 2, 0, pl, 2);
        vif_test_expect("V3-8 elem 0", 0, 1u, 2u, 3u, 0);
        vif_test_expect("V3-8 elem 1 is byte-packed", 1,
                        4u, 5u, 6u, 0);
    }
    {
        static const u32 pl[1] = {0x5Au};
        u32 saved_tops = vif[1].tops;
        vif[1].tops = 8;
        vif_test_run(0x60, 1, 4u, pl, 1);
        vif_test_expect("FLG=0 ignores TOPS", 4, 0x5Au, 0x5Au, 0x5Au, 0x5Au);
        vif_test_run(0x60, 1, (1u << 15) | 4u, pl, 1);
        vif_test_expect("FLG=1 adds TOPS", 12, 0x5Au, 0x5Au, 0x5Au, 0x5Au);
        vif[1].tops = saved_tops;
    }


    {
        const u32 pl[] = {1,2,3,4};
        vif[1].mode=2; vif[1].mask=0;
        memset(vif[1].row,0,16);
        vif_test_run(0x64,2,0,pl,4);
        vif_test_expect("V2 MODE2 first",0,1,2,1,2);
        vif_test_expect("V2 MODE2 accumulated",1,4,6,4,6);
        if (vif[1].row[2]!=4 || vif[1].row[3]!=6) vif_test_fail++;
        vif[1].mode=0;
    }
    {
        memset(vu1.mem,0xCD,vu1.mem_size);
        ps2_vif_write(0x85,0x64010000);
        ps2_vif_write(0x87,10);
        ps2_reg128 q={{0}}; q.uw[0]=20;
        ps2_vif_fifo(1,&q);
        vif_test_expect("TTE V2-32 physical alignment",0,10,20,10,0);
    }
    {
        memset(vu1.mem,0xCD,vu1.mem_size);
        ps2_vif_write(0x87,0x68010000);
        ps2_vif_write(1,1); ps2_vif_write(1,2); ps2_vif_write(1,3);
        ps2_vif_write(1,0x07001234);
        vif_test_expect("V3-32 next-code lookahead",0,1,2,3,0x07001234);
        if (vif[1].mark!=0x1234) { vif_test_fail++; ps2_log("FAIL V3 swallowed MARK"); }
    }
    vif[1].mask = saved_mask;
    vif[1].mode = saved_mode;
    memcpy(vif[1].row, saved_row, 16);
    memcpy(vif[1].col, saved_col, 16);
    memset(vu1.mem, 0, vu1.mem_size);
    vif[1].wait = 0;
    vif[1].accn = 0;
    if (vif_test_fail) ps2_log("---- VIF UNPACK self-test: %u FAILURES ----",
                               vif_test_fail);
    else ps2_log("---- VIF UNPACK self-test: all cases pass ----");
    return vif_test_fail ? 1 : 0;
}

static u32 vu_test_fail;

static void vu_test_expect(const char *name, u32 got, u32 want) {
    if (got == want) return;
    vu_test_fail++;
    ps2_log("  FAIL %-42s got %u, want %u", name, got, want);
}

#define VU_UP_NOP 0x000002FFu
#define VU_LO_NOP 0x8000033Cu
static u32 vu_up_fmac(u32 funct, u32 dest, u32 fd, u32 fs, u32 ft) {
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
static u32 vu_lo_fmand(u32 it, u32 is) {
    return (0x1Au << 25) | (it << 16) | (is << 11);
}

#define VU_LO_SPECIAL 0x80000000u
#define VU_LO_WAITQ   (VU_LO_SPECIAL | 0x3BFu)
#define VU_LO_WAITP   (VU_LO_SPECIAL | 0x7BFu)
static u32 vu_lo_div(u32 fsf, u32 ftf, u32 fs, u32 ft) {
    return VU_LO_SPECIAL | (ftf << 23) | (fsf << 21)
         | (ft << 16) | (fs << 11) | 0x3BCu;
}
static u32 vu_lo_esqrt(u32 fsf, u32 fs) {
    return VU_LO_SPECIAL | (fsf << 21) | (fs << 11) | 0x7BCu;
}
static u32 vu_lo_mfp(u32 dest, u32 ft) {
    return VU_LO_SPECIAL | (dest << 21) | (ft << 16) | 0x67Cu;
}
static u32 vu_up_mulq(u32 dest, u32 fd, u32 fs) {
    return vu_up_fmac(0x1Cu, dest, fd, fs, 0);
}

static void vu_test_prog(const u32 *lo, const u32 *up, unsigned n) {
    unsigned i;
    memset(vu1.micro, 0, vu1.micro_size);
    for (i = 0; i < n; i++) {
        memcpy(vu1.micro + i * 8u, &lo[i], 4);
        memcpy(vu1.micro + i * 8u + 4u, &up[i], 4);
    }
}

static void vu_test_reset(void) {
    memset(vu1.vi, 0, sizeof vu1.vi);
    memset(vu1.fpipe, 0, sizeof vu1.fpipe);
    vu1.mac = 0;
    vu1.status = 0;
    vu1.q = 0.0f; vu1.p = 0.0f;
    vu1.q_pend = 0.0f; vu1.p_pend = 0.0f;
    vu1.q_ready = 0; vu1.p_ready = 0;
    vu1.vf[0].f[0] = 0.0f; vu1.vf[0].f[1] = 0.0f;
    vu1.vf[0].f[2] = 0.0f; vu1.vf[0].f[3] = 1.0f;
}

int ps2_vu_selftest(void) {
    vu_test_fail = 0;
    if (!vu1.micro || !vu1.mem) { ps2_log("vu selftest: VU1 not initialised"); return 1; }
    {
        vu_test_reset();
        vu1.vi[15] = 0xBEEF;
        vu1.clip = 1;
        vu_lower(&vu1, 0x24000FFFu, 0, 0);
        vu_test_expect("FCAND writes fixed VI01", vu1.vi[1], 1);
        vu_test_expect("FCAND preserves VI00", vu1.vi[0], 0);
        vu1.clip = 0x41;
        vu_lower(&vu1, 0x26FFFFBEu, 0, 0);
        vu_test_expect("FCOR writes fixed VI01", vu1.vi[1], 1);
        vu_test_expect("FCOR preserves VI15", vu1.vi[15], 0xBEEF);
        vu_lower(&vu1, 0x20FFFFBEu, 0, 0);
        vu_test_expect("FCEQ false", vu1.vi[1], 0);
        vu1.clip = 0xFFFFBE;
        vu_lower(&vu1, 0x20FFFFBEu, 0, 0);
        vu_test_expect("FCEQ true", vu1.vi[1], 1);
        vu_test_expect("FCEQ preserves VI15", vu1.vi[15], 0xBEEF);
    }
    ps2_log("---- VU flag-pipeline self-test "
            "(VU_Users_Manual.pdf 3.4.8, figure 3-16) ----");

    {
        u32 lo[8], up[8];
        unsigned i;
        for (i = 0; i < 8; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
        up[1] = vu_up_fmac(0x2Cu, 0xEu, 3, 1, 2);
        lo[4] = vu_lo_fmand(5, 2);
        lo[5] = vu_lo_fmand(6, 2);
        up[6] |= 0x40000000u;
        vu_test_prog(lo, up, 8);
        vu_test_reset();
        vu1.vi[2] = 224;
        for (i = 0; i < 4; i++) { vu1.vf[1].f[i] = -1.0f; vu1.vf[2].f[i] = 1.0f; }
        vu_run(&vu1, 0);
        vu_test_expect("flag not visible 3 instructions later", vu1.vi[5], 0u);
        vu_test_expect("flag visible 4 instructions later", vu1.vi[6], 224u);
    }

    {
        u32 lo[16], up[16];
        unsigned i;
        for (i = 0; i < 16; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
        up[1] = vu_up_fmac(0x28u, 0xEu, 3, 1, 2);
        up[3] = vu_up_fmac(0x2Cu, 0xEu, 4, 1, 2);
        lo[5] = vu_lo_fmand(5, 2);
        lo[9] = vu_lo_fmand(6, 2);
        up[10] |= 0x40000000u;
        vu_test_prog(lo, up, 16);
        vu_test_reset();
        vu1.vi[2] = 0xFFu;
        for (i = 0; i < 4; i++) { vu1.vf[1].f[i] = -1.0f; vu1.vf[2].f[i] = 1.0f; }
        vu_run(&vu1, 0);
        vu_test_expect("first FMAND reads the first FMAC", vu1.vi[5], 0x0Eu);
        vu_test_expect("second FMAND reads the second FMAC", vu1.vi[6], 0xE0u);
    }

    {
        u32 lo[8], up[8];
        unsigned i;
        for (i = 0; i < 8; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
        up[1] = vu_up_fmac(0x2Cu, 0xEu, 3, 1, 2);
        up[2] |= 0x40000000u;
        lo[5] = vu_lo_fmand(7, 2);
        vu_test_prog(lo, up, 8);
        vu_test_reset();
        vu1.vi[2] = 224;
        for (i = 0; i < 4; i++) { vu1.vf[1].f[i] = -1.0f; vu1.vf[2].f[i] = 1.0f; }
        vu_run(&vu1, 0);
        vu_test_expect("pipeline drains at program end", vu1.mac & 0xE0u, 0xE0u);
    }

    {
        u32 lo[16], up[16];
        unsigned i;
        for (i = 0; i < 16; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
        up[1] = vu_up_fmac(0x2Cu, 0xEu, 3, 1, 2);
        up[2] = vu_up_fmac(0x2Bu, 0xEu, 5, 2, 2);
        lo[5] = vu_lo_fmand(5, 2);
        up[6] |= 0x40000000u;
        vu_test_prog(lo, up, 16);
        vu_test_reset();
        vu1.vi[2] = 224;
        for (i = 0; i < 4; i++) { vu1.vf[1].f[i] = -1.0f; vu1.vf[2].f[i] = 1.0f; }
        vu_run(&vu1, 0);
        vu_test_expect("MAX between FMAC and FMAND changes nothing",
                       vu1.vi[5], 224u);
    }

    ps2_log("---- VU Q/P latency self-test "
            "(VU_Users_Manual.pdf 4.3, 4.4) ----");
    if (!vu_qlat_on()) {
        ps2_log("  skipped: PS2_VU_QLAT=0 publishes Q and P immediately");
    } else {
        static const float Q_OLD = 9.0f, Q_NEW = 0.5f;
        unsigned i;

        {
            u32 lo[16], up[16];
            for (i = 0; i < 16; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
            lo[1] = vu_lo_div(3, 3, 1, 2);
            up[7] = vu_up_mulq(0xFu, 5, 3);
            up[8] = vu_up_mulq(0xFu, 6, 3);
            up[10] |= 0x40000000u;
            vu_test_prog(lo, up, 16);
            vu_test_reset();
            vu1.q = Q_OLD;
            vu1.vf[1].f[3] = 1.0f;
            vu1.vf[2].f[3] = 2.0f;
            for (i = 0; i < 4; i++) vu1.vf[3].f[i] = 1.0f;
            vu_run(&vu1, 0);
            vu_test_expect("Q not visible 6 cycles after DIV",
                           (u32)vu1.vf[5].f[0], (u32)Q_OLD);
            vu_test_expect("Q visible 7 cycles after DIV",
                           (u32)(vu1.vf[6].f[0] * 10.0f), (u32)(Q_NEW * 10.0f));
        }

        {
            u32 lo[16], up[16];
            for (i = 0; i < 16; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
            lo[1] = vu_lo_div(3, 3, 1, 2);
            lo[3] = VU_LO_WAITQ;
            up[4] = vu_up_mulq(0xFu, 5, 3);
            up[6] |= 0x40000000u;
            vu_test_prog(lo, up, 16);
            vu_test_reset();
            vu1.q = Q_OLD;
            vu1.vf[1].f[3] = 1.0f;
            vu1.vf[2].f[3] = 2.0f;
            for (i = 0; i < 4; i++) vu1.vf[3].f[i] = 1.0f;
            vu_run(&vu1, 0);
            vu_test_expect("WAITQ makes Q visible immediately",
                           (u32)(vu1.vf[5].f[0] * 10.0f), (u32)(Q_NEW * 10.0f));
        }

        {
            u32 lo[8], up[8];
            for (i = 0; i < 8; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
            lo[1] = vu_lo_div(3, 3, 1, 2);
            up[2] |= 0x40000000u;
            vu_test_prog(lo, up, 8);
            vu_test_reset();
            vu1.q = Q_OLD;
            vu1.vf[1].f[3] = 1.0f;
            vu1.vf[2].f[3] = 2.0f;
            vu_run(&vu1, 0);
            vu_test_expect("FDIV unit drains at program end",
                           (u32)(vu1.q * 10.0f), (u32)(Q_NEW * 10.0f));
        }

        {
            u32 lo[24], up[24];
            for (i = 0; i < 24; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
            lo[1] = vu_lo_esqrt(3, 1);
            lo[5] = vu_lo_mfp(0xFu, 5);
            lo[13] = vu_lo_mfp(0xFu, 6);
            up[15] |= 0x40000000u;
            vu_test_prog(lo, up, 24);
            vu_test_reset();
            vu1.p = 7.0f;
            vu1.vf[1].f[3] = 16.0f;
            vu_run(&vu1, 0);
            vu_test_expect("MFP does not wait for P", (u32)vu1.vf[5].f[0], 7u);
            vu_test_expect("P visible 12 cycles after ESQRT",
                           (u32)vu1.vf[6].f[0], 4u);
        }

        {
            u32 lo[16], up[16];
            for (i = 0; i < 16; i++) { lo[i] = VU_LO_NOP; up[i] = VU_UP_NOP; }
            lo[1] = vu_lo_esqrt(3, 1);
            lo[3] = VU_LO_WAITP;
            lo[4] = vu_lo_mfp(0xFu, 5);
            up[6] |= 0x40000000u;
            vu_test_prog(lo, up, 16);
            vu_test_reset();
            vu1.p = 7.0f;
            vu1.vf[1].f[3] = 16.0f;
            vu_run(&vu1, 0);
            vu_test_expect("WAITP makes P visible immediately",
                           (u32)vu1.vf[5].f[0], 4u);
        }
    }

    memset(vu1.micro, 0, vu1.micro_size);
    vu_test_reset();
    if (vu_test_fail) ps2_log("---- VU self-test: %u FAILURES ----",
                              vu_test_fail);
    else ps2_log("---- VU self-test: all cases pass ----");
    return vu_test_fail ? 1 : 0;
}
