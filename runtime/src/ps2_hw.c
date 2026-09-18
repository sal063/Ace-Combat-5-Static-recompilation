#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_capture.h"
#include "ps2_gfxq.h"
#include "rn.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

u8 *ps2_vu1_memory(void);
u8 *ps2_vu1_microcode(void);
void ps2_sif_reg_write(u32 addr, u32 val);
u32  ps2_sif_reg_read(u32 addr);
void ps2_gs_write_reg(u32 reg, u64 val);
void ps2_gs_hwreg_pair(u64 lo, u64 hi);
void ps2_gs_hwreg_stream(const void *data, u32 qwc);
void ps2_gs_priv_write(u32 addr, u64 val);
u64  ps2_gs_priv_read(u32 addr);
void ps2_vif_write(int which, u32 word);
void ps2_vif_fifo(int which, const ps2_reg128 *q);
int  ps2_vif_stalled(int which);
void ps2_vif_clear_stall(int which);
void ps2_vif_fbrst(int which, u32 val);
void ps2_vif_note_tag(int ch, u32 tadr, u32 tag_lo, u32 tag_hi);
void ps2_intc_raise(int irq);
u32  ps2_ipu_read32(u32 addr);
void ps2_ipu_write32(u32 addr, u32 value);
void ps2_ipu_dma_in(const void *qw, u32 n);
void ps2_ipu_dma_out(void *dst, u32 n);
u32  ps2_ipu_in_space(void);
u32  ps2_ipu_out_avail(void);
u32  ps2_ipu_want(void);

typedef struct {
    u32 chcr, madr, qwc, tadr, asr0, asr1, sadr;
} ps2_dma_ch;

static ps2_dma_ch dma[10];

#define IPU_CH4_LOG 32
static struct { u32 chcr, madr, qwc, tadr; } ch4_log[IPU_CH4_LOG];
static u32 ch4_log_n;

static void ch4_log_push(u32 v) {
    u32 i = ch4_log_n++ % IPU_CH4_LOG;
    ch4_log[i].chcr = v;         ch4_log[i].madr = dma[4].madr;
    ch4_log[i].qwc  = dma[4].qwc; ch4_log[i].tadr = dma[4].tadr;
}
static u32 d_ctrl, d_stat, d_pcr, d_sqwc, d_rbsr, d_rbor, d_stadr, d_enable;
static u32 intc_stat, intc_mask;

void ps2_kernel_poll_vblank(void);

#define T_CLKS_M   0x0003u
#define T_ZRET_M   0x0040u
#define T_CUE_M    0x0080u
#define T_CMPE_M   0x0100u
#define T_OVFE_M   0x0200u
#define T_EQUF_M   0x0400u
#define T_OVFF_M   0x0800u

#define EE_BUSCLK_HZ   147456000ull
#define NTSC_HBLANK_HZ     15734ull

static u32 timer_count[4], timer_mode[4], timer_comp[4], timer_hold[4];
static u64 timer_last_ns[4], timer_frac[4];
static u64 timer_reads, timer_irqs;
static u64 timer_ticks_made[4];

static u64 host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u64 timer_rate(u32 mode) {
    switch (mode & T_CLKS_M) {
    case 0:  return EE_BUSCLK_HZ;
    case 1:  return EE_BUSCLK_HZ / 16ull;
    case 2:  return EE_BUSCLK_HZ / 256ull;
    default: return NTSC_HBLANK_HZ;
    }
}

#define FIELD_NS_DET 16683333ull
static u64 timer_det_fields;

static void timer_sync(int ch) {
    u64 now = host_ns(), elapsed, num, ticks, modulus, wraps;
    u32 mode = timer_mode[ch];
    u32 old = timer_count[ch] & 0xFFFFu;
    u32 comp = timer_comp[ch] & 0xFFFFu;
    int matched = 0, overflowed = 0, edge = 0;

    if (timer_last_ns[ch] == 0) timer_last_ns[ch] = now;
    {   static int det = -1;
        if (det < 0) det = getenv("PS2_TIMER_DET") ? 1 : 0;
        if (det) {
            static u64 seen[4], used[4];
            static u32 reads[4], reads_prev[4];
            if (seen[ch] != timer_det_fields) {
                seen[ch] = timer_det_fields;
                reads_prev[ch] = reads[ch] ? reads[ch] : 1u;
                reads[ch] = 0;
                elapsed = (used[ch] < FIELD_NS_DET) ? FIELD_NS_DET - used[ch] : 0;
                used[ch] = 0;
            } else {
                u32 rp = reads_prev[ch] ? reads_prev[ch] : 1u;
                u64 share = FIELD_NS_DET / rp;
                reads[ch]++;
                if (used[ch] + share > FIELD_NS_DET)
                    share = FIELD_NS_DET - used[ch];
                used[ch] += share;
                elapsed = share;
            }
            goto have_elapsed;
        }
    }
    elapsed = now - timer_last_ns[ch];
    timer_last_ns[ch] = now;
have_elapsed:
    if (!(mode & T_CUE_M)) { timer_frac[ch] = 0; return; }
    if (elapsed > 1000000000ull) elapsed = 1000000000ull;

    num = elapsed * timer_rate(mode) + timer_frac[ch];
    ticks = num / 1000000000ull;
    timer_frac[ch] = num % 1000000000ull;
    timer_ticks_made[ch] += ticks;
    if (!ticks) return;

    modulus = (mode & T_ZRET_M) ? (u64)comp + 1ull : 0x10000ull;
    if (modulus == 0) modulus = 0x10000ull;
    wraps = ((u64)old + ticks) / modulus;
    timer_count[ch] = (u32)(((u64)old + ticks) % modulus);

    if (mode & T_ZRET_M) {
        matched = wraps > 0;
    } else {
        overflowed = wraps > 0;
        if (wraps > 0) matched = 1;
        else matched = (old < comp && (u64)old + ticks >= (u64)comp);
    }
    if (matched && !(timer_mode[ch] & T_EQUF_M)) {
        timer_mode[ch] |= T_EQUF_M;
        if (mode & T_CMPE_M) edge = 1;
    }
    if (overflowed && !(timer_mode[ch] & T_OVFF_M)) {
        timer_mode[ch] |= T_OVFF_M;
        if (mode & T_OVFE_M) edge = 1;
    }
    if (edge) {
        ps2_intc_raise(9 + ch);
        timer_irqs++;
    }
}

static void timer_write_mode(int ch, u32 v) {
    u32 keep = timer_mode[ch] & (T_EQUF_M | T_OVFF_M);
    timer_sync(ch);
    keep &= ~(v & (T_EQUF_M | T_OVFF_M));
    timer_mode[ch] = (v & ~(T_EQUF_M | T_OVFF_M)) | keep;
    timer_last_ns[ch] = host_ns();
    timer_frac[ch] = 0;
}

void ps2_timers_tick(void) {
    timer_det_fields++;
    for (int i = 0; i < 4; i++) timer_sync(i);
    if (getenv("PS2_TIMER_TRACE")) {
        static u64 field;
        static u32 prev[4];
        static u64 sum[4];
        int i;
        for (i = 0; i < 4; i++) {
            u32 d = (timer_count[i] - prev[i]) & 0xFFFFu;
            sum[i] += d;
            prev[i] = timer_count[i];
        }
        (void)sum;
        if (++field % 60 == 0 && field <= 1800) {
            static u64 pm[4];
            ps2_log("timertrace: field %llu  ticks/field T0=%.1f T1=%.1f T2=%.1f"
                    " T3=%.1f   (expect 262.5 on HBLANK; mode T0=%04X T1=%04X)",
                    (unsigned long long)field,
                    (timer_ticks_made[0] - pm[0]) / 60.0,
                    (timer_ticks_made[1] - pm[1]) / 60.0,
                    (timer_ticks_made[2] - pm[2]) / 60.0,
                    (timer_ticks_made[3] - pm[3]) / 60.0,
                    timer_mode[0], timer_mode[1]);
            for (i = 0; i < 4; i++) pm[i] = timer_ticks_made[i];
        }
    }
}

void ps2_timers_report(void) {
    if (!timer_reads && !timer_irqs) return;
    ps2_log("timers: %llu counter reads, %llu interrupts raised "
            "(T0 mode=%04X comp=%u, T1 mode=%04X comp=%u)",
            (unsigned long long)timer_reads, (unsigned long long)timer_irqs,
            timer_mode[0], timer_comp[0], timer_mode[1], timer_comp[1]);
}

static u32 gif_ctrl, gif_mode, gif_stat;
static u64 gs_csr = 0x0000000000000000ull;
static u64 gs_imr = 0x7F00ull;

#define GS_CSR_VSINT (1ull << 3)
#define GS_CSR_RESET (1ull << 9)
#define GS_CSR_FIELD (1ull << 13)
#define PS2_INTC_VBLANK_S 2
#define PS2_INTC_VBLANK_E 3

u32 ps2_intc_stat(void) { return intc_stat; }
u32 ps2_intc_mask(void) { return intc_mask; }
void ps2_intc_raise(int irq) { intc_stat |= 1u << irq; }
void ps2_intc_ack(int irq) { intc_stat &= ~(1u << irq); }
u32 ps2_intc_pending(void) {
    return intc_stat & intc_mask & ~0x0Cu;
}

static u32 gif_q_latch;

extern int ps2_gif_path;

static void gif_xyz_probe(u32 reg, const ps2_reg128 *q) {
    static int armed_prev;
    static unsigned left;
    if (ps2_diag_armed && !armed_prev) left = 8u;
    armed_prev = ps2_diag_armed;
    if (!ps2_diag_armed || !left) return;
    left--;
    ps2_log("gif xyz: PATH%d reg %02X  qw %08X %08X %08X %08X  ->  "
            "X=%u (%.1f px)  Y=%u (%.1f px)",
            ps2_gif_path, reg, q->uw[0], q->uw[1], q->uw[2], q->uw[3],
            q->uw[0] & 0xFFFFu, (double)(q->uw[0] & 0xFFFFu) / 16.0,
            q->uw[1] & 0xFFFFu, (double)(q->uw[1] & 0xFFFFu) / 16.0);
}

static void gif_packed(u32 reg, const ps2_reg128 *q) {
    switch (reg) {
    case 0x00:
        ps2_gs_write_reg(0x00, q->ud[0] & 0x7FFull);
        break;
    case 0x01:
        ps2_gs_write_reg(0x01,
            ((u64)(q->uw[0] & 0xFFu))
          | ((u64)(q->uw[1] & 0xFFu) << 8)
          | ((u64)(q->uw[2] & 0xFFu) << 16)
          | ((u64)(q->uw[3] & 0xFFu) << 24)
          | ((u64)gif_q_latch << 32));
        break;
    case 0x02:
        gif_q_latch = q->uw[2];
        ps2_gs_write_reg(0x02, q->ud[0]);
        break;
    case 0x03:
        ps2_gs_write_reg(0x03, ((u64)(q->uw[0] & 0x3FFFu))
                             | ((u64)(q->uw[1] & 0x3FFFu) << 16));
        break;
    case 0x04:
        gif_xyz_probe(reg, q);
        ps2_gs_write_reg((q->uw[3] & 0x8000u) ? 0x0Cu : 0x04u,
            ((u64)(q->uw[0] & 0xFFFFu))
          | ((u64)(q->uw[1] & 0xFFFFu) << 16)
          | ((u64)((q->uw[2] >> 4) & 0xFFFFFFu) << 32)
          | ((u64)((q->uw[3] >> 4) & 0xFFu) << 56));
        break;
    case 0x05:
        gif_xyz_probe(reg, q);
        ps2_gs_write_reg((q->uw[3] & 0x8000u) ? 0x0Du : 0x05u,
            ((u64)(q->uw[0] & 0xFFFFu))
          | ((u64)(q->uw[1] & 0xFFFFu) << 16)
          | ((u64)q->uw[2] << 32));
        break;
    case 0x0A:
        ps2_gs_write_reg(0x0A, (u64)((q->uw[3] >> 4) & 0xFFu) << 56);
        break;
    case 0x0E: {
        u32 r = (u32)(q->ud[1] & 0xFFull);
        ps2_gs_write_reg(r, q->ud[0]);
        break;
    }
    case 0x0F:
        break;
    default:
        ps2_gs_write_reg(reg, q->ud[0]);
        break;
    }
}

void ps2_gif_transfer(const ps2_reg128 *data, u32 qwc);
void ps2_gif_reset(void);

int ps2_gif_path;

static u64 gif_tag_hist[4];
static u64 gif_tag_qwc[4];

static u64 gif_runaway, gif_abandoned;

void ps2_gif_tag_report(void) {
    ps2_log("gif tags: packed=%llu(%lluqw) reglist=%llu(%lluqw) "
            "image=%llu(%lluqw) disabled=%llu(%lluqw)",
            (unsigned long long)gif_tag_hist[0], (unsigned long long)gif_tag_qwc[0],
            (unsigned long long)gif_tag_hist[1], (unsigned long long)gif_tag_qwc[1],
            (unsigned long long)gif_tag_hist[2], (unsigned long long)gif_tag_qwc[2],
            (unsigned long long)gif_tag_hist[3], (unsigned long long)gif_tag_qwc[3]);
    ps2_log("gif walker: %llu implausible tags, %llu packets discarded "
            "mid-tag by XGKICK", (unsigned long long)gif_runaway,
            (unsigned long long)gif_abandoned);
}

static struct {
    int active;
    u32 flg, nreg, nloop;
    u32 eop;
    u64 regs;
    u32 n;
    u32 r;
} gif;

void ps2_gif_reset(void) { gif.active = 0; }

void ps2_gif_cap_save(ps2_gif_capstate *st) {
    memset(st, 0, sizeof *st);
    st->active = (u32)gif.active;
    st->flg = gif.flg;
    st->nreg = gif.nreg;
    st->nloop = gif.nloop;
    st->eop = gif.eop;
    st->n = gif.n;
    st->r = gif.r;
    st->regs = gif.regs;
    st->q_latch = gif_q_latch;
    st->path = (u32)ps2_gif_path;
    st->ctrl = gif_ctrl;
    st->mode = gif_mode;
    st->stat = gif_stat;
}

void ps2_gif_cap_load(const ps2_gif_capstate *st) {
    gif.active = (int)st->active;
    gif.flg = st->flg;
    gif.nreg = st->nreg;
    gif.nloop = st->nloop;
    gif.eop = st->eop;
    gif.n = st->n;
    gif.r = st->r;
    gif.regs = st->regs;
    gif_q_latch = st->q_latch;
    ps2_gif_path = (int)st->path;
    gif_ctrl = st->ctrl;
    gif_mode = st->mode;
    gif_stat = st->stat;
}

static void gif_reglist_item(u32 desc, u64 data) {
    if (desc == 0x0Eu || desc == 0x0Fu) return;
    ps2_gs_write_reg(desc, data);
}

static void gif_feed(const ps2_reg128 *q) {
    if (!gif.active) {
        u64 tag = q->ud[0];
        u32 pre = (u32)((tag >> 46) & 1u);
        u32 prim = (u32)((tag >> 47) & 0x7FFu);
        gif.eop = (u32)((tag >> 15) & 1u);
        gif.nloop = (u32)(tag & 0x7FFFu);
        gif.flg = (u32)((tag >> 58) & 3u);
        gif.nreg = (u32)((tag >> 60) & 0xFu);
        gif.regs = q->ud[1];
        if (gif.nreg == 0) gif.nreg = 16;
        if (pre && gif.flg == 0) ps2_gs_write_reg(0x00, prim);
        gif_tag_hist[gif.flg]++;
        gif_tag_qwc[gif.flg] += gif.nloop;
        {
            u32 payload = (gif.flg <= 1u) ? gif.nloop * gif.nreg : gif.nloop;
            if (payload > 262144u) {
                gif_runaway++;
                if (gif_runaway <= 12u)
                    ps2_log("gif: implausible tag on PATH%d -- flg=%u nreg=%u "
                            "nloop=%u payload=%u qw, raw %016llX %016llX",
                            ps2_gif_path, gif.flg, gif.nreg, gif.nloop, payload,
                            (unsigned long long)q->ud[0],
                            (unsigned long long)q->ud[1]);
            }
        }
        gif.n = 0;
        gif.r = 0;
        gif.active = gif.nloop != 0;
        return;
    }

    switch (gif.flg) {
    case 0:
        gif_packed((u32)((gif.regs >> (gif.r * 4)) & 0xFu), q);
        if (++gif.r >= gif.nreg) {
            gif.r = 0;
            if (++gif.n >= gif.nloop) gif.active = 0;
        }
        break;
    case 1: {
        u32 total = gif.nloop * gif.nreg;
        gif_reglist_item((u32)((gif.regs >> ((gif.n % gif.nreg) * 4)) & 0xFu),
                         q->ud[0]);
        gif.n++;
        if (gif.n < total) {
            gif_reglist_item((u32)((gif.regs >> ((gif.n % gif.nreg) * 4)) & 0xFu),
                             q->ud[1]);
            gif.n++;
        }
        if (gif.n >= total) gif.active = 0;
        break;
    }
    case 2:
        ps2_gs_hwreg_pair(q->ud[0], q->ud[1]);
        if (++gif.n >= gif.nloop) gif.active = 0;
        break;
    default:
        if (++gif.n >= gif.nloop) gif.active = 0;
        break;
    }
}

void ps2_gif_transfer(const ps2_reg128 *data, u32 qwc) {
    PS2_PHASE_BEGIN(PS2_PH_GIF);
    if (!ps2_gif_path) ps2_gif_path = 3;
    for (u32 i = 0; i < qwc;) {
        if (gif.active && gif.flg == 2u && !ps2_diag_armed && !g_cap_shallow) {
            u32 n = gif.nloop - gif.n;
            if (n > qwc - i) n = qwc - i;
            ps2_gs_hwreg_stream((const u8 *)data + (size_t)i * 16u, n);
            gif.n += n;
            if (gif.n >= gif.nloop) gif.active = 0;
            i += n;
        } else {
            ps2_reg128 qw;
            memcpy(&qw, (const u8 *)data + (size_t)i * 16u, sizeof(qw));
            gif_feed(&qw);
            i++;
        }
    }
    PS2_PHASE_END(PS2_PH_GIF);
}

u32 ps2_gif_transfer_eop(const ps2_reg128 *data, u32 max) {
    u32 i;
    if (gif.active) {
        gif_abandoned++;
        if (gif_abandoned <= 8u)
            ps2_log("gif: XGKICK discarded a packet %u/%u loops into a flg=%u "
                    "nreg=%u tag -- that stream will resume mid-payload",
                    gif.n, gif.nloop, gif.flg, gif.nreg);
    }
    gif.active = 0;
    for (i = 0; i < max; i++) {
        int was_active = gif.active;
        gif_feed(&data[i]);
        if (gif.eop && !gif.active && (was_active || gif.nloop == 0))
            return i + 1;
    }
    return max;
}

static u32 gif_dump_left, gif_dump_skip;
static u64 gif_kick_empty, gif_kick_drawn;

void ps2_gif_dump_arm(u32 packets) {
    const char *e = getenv("PS2_GIF_DUMP_SKIP");
    gif_dump_left = packets;
    gif_dump_skip = e ? (u32)strtoul(e, NULL, 0) : 0;
}

void ps2_gif_kick_report(void) {
    u64 tot = gif_kick_empty + gif_kick_drawn;
    if (!tot) return;
    ps2_log("PATH1 XGKICK packets: %llu carried geometry, %llu were empty "
            "(%.1f%% empty)", (unsigned long long)gif_kick_drawn,
            (unsigned long long)gif_kick_empty,
            100.0 * (double)gif_kick_empty / (double)tot);
}

static int gif_packet_empty(const u8 *vumem, u32 qwords, u32 addr_qw) {
    const ps2_reg128 *q =
        (const ps2_reg128 *)(vumem + ((addr_qw % qwords) * 16u));
    return (u32)(q->ud[0] & 0x7FFFull) == 0u;
}

static void gif_dump_packed(u32 reg, const ps2_reg128 *q, u32 vtx) {
    switch (reg) {
    case 0x00:
        ps2_log("        PRIM   %03X", (u32)(q->ud[0] & 0x7FFull));
        break;
    case 0x01:
        ps2_log("        RGBAQ  r=%-3u g=%-3u b=%-3u a=%-3u",
                q->uw[0] & 0xFFu, q->uw[1] & 0xFFu,
                q->uw[2] & 0xFFu, q->uw[3] & 0xFFu);
        break;
    case 0x02: {
        float s, t, qq;
        memcpy(&s, &q->uw[0], 4); memcpy(&t, &q->uw[1], 4);
        memcpy(&qq, &q->uw[2], 4);
        ps2_log("        ST     s=%-12g t=%-12g q=%-12g  -> uv %g,%g",
                (double)s, (double)t, (double)qq,
                qq != 0.0f ? (double)(s / qq) : 0.0, qq != 0.0f ? (double)(t / qq) : 0.0);
        break;
    }
    case 0x03:
        ps2_log("        UV     u=%.1f v=%.1f", (double)(q->uw[0] & 0x3FFFu) / 16.0,
                (double)(q->uw[1] & 0x3FFFu) / 16.0);
        break;
    case 0x04:
    case 0x05: {
        u32 adc = q->uw[3] & 0x8000u;
        u32 z = reg == 0x05 ? q->uw[2] : ((q->uw[2] >> 4) & 0xFFFFFFu);
        if (reg == 0x04)
            ps2_log("     %2u XYZF%s x=%-9.2f y=%-9.2f z=%-10u F=%-3u%s", vtx,
                    adc ? "3" : "2",
                    (double)(q->uw[0] & 0xFFFFu) / 16.0,
                    (double)(q->uw[1] & 0xFFFFu) / 16.0, z,
                    (q->uw[3] >> 4) & 0xFFu,
                    adc ? "  (ADC: no draw kick)" : "");
        else
            ps2_log("     %2u XYZ%s   x=%-9.2f y=%-9.2f z=%-10u%s", vtx,
                    adc ? "3" : "2",
                    (double)(q->uw[0] & 0xFFFFu) / 16.0,
                    (double)(q->uw[1] & 0xFFFFu) / 16.0, z,
                    adc ? "  (ADC: no draw kick)" : "");
        break;
    }
    case 0x0E:
        ps2_log("        A+D    reg %02X = %016llX", (u32)(q->ud[1] & 0xFFull),
                (unsigned long long)q->ud[0]);
        break;
    case 0x0F: break;
    default:
        ps2_log("        reg%02X  %016llX", reg, (unsigned long long)q->ud[0]);
        break;
    }
}

static void gif_dump_packet(const u8 *vumem, u32 qwords, u32 addr_qw) {
    u32 i, nloop = 0, nreg = 0, flg = 0, r = 0, n = 0, vtx = 0, eop = 0;
    u64 regs = 0;
    int active = 0, shown = 0;
    for (i = 0; i < qwords && shown < 40; i++) {
        const ps2_reg128 *q =
            (const ps2_reg128 *)(vumem + (((addr_qw + i) % qwords) * 16u));
        if (!active) {
            u64 tag = q->ud[0];
            nloop = (u32)(tag & 0x7FFFull);
            flg = (u32)((tag >> 58) & 3ull);
            nreg = (u32)((tag >> 60) & 0xFull);
            if (!nreg) nreg = 16;
            regs = q->ud[1];
            ps2_log("     GIFtag nloop=%u eop=%u pre=%u prim=%03X flg=%s nreg=%u "
                    "regs=%016llX", nloop, (u32)((tag >> 15) & 1ull),
                    (u32)((tag >> 46) & 1ull), (u32)((tag >> 47) & 0x7FFull),
                    flg == 0 ? "PACKED" : flg == 1 ? "REGLIST"
                             : flg == 2 ? "IMAGE" : "disabled", nreg,
                    (unsigned long long)regs);
            shown++;
            n = 0; r = 0; vtx = 0;
            eop = (u32)((tag >> 15) & 1ull);
            active = nloop != 0;
            if (!active && eop) return;
            continue;
        }
        if (flg == 0) {
            u32 reg = (u32)((regs >> (r * 4)) & 0xFu);
            if (reg == 0x04 || reg == 0x05) vtx++;
            gif_dump_packed(reg, q, vtx);
            shown++;
            if (++r >= nreg) { r = 0; if (++n >= nloop) active = 0; }
        } else {
            shown++;
            if (++n >= nloop) active = 0;
        }
        if (!active && eop) return;
    }
}

u32 ps2_gif_kick(const u8 *vumem, u32 mem_size, u32 addr_qw) {
    u32 qwords = mem_size / 16u;
    u32 i;
    PS2_PHASE_BEGIN(PS2_PH_GIF);
    ps2_gif_path = 1;
    if (!vumem || !qwords) { ps2_gif_path = 0; PS2_PHASE_END(PS2_PH_GIF); return 0; }
    gif.active = 0;
    {
        int empty = gif_packet_empty(vumem, qwords, addr_qw);
        if (empty) gif_kick_empty++; else gif_kick_drawn++;
        if (gif_dump_left && ps2_diag_armed && !empty && gif_dump_skip) {
            gif_dump_skip--;
        } else if (gif_dump_left && ps2_diag_armed && !empty) {
            gif_dump_left--;
            ps2_log("   PATH1 XGKICK at qw %u:", addr_qw);
            gif_dump_packet(vumem, qwords, addr_qw);
        }
    }
    for (i = 0; i < qwords; i++) {
        const ps2_reg128 *q =
            (const ps2_reg128 *)(vumem + (((addr_qw + i) % qwords) * 16u));
        int was_active = gif.active;
        gif_feed(q);
        if (gif.eop && !gif.active && (was_active || gif.nloop == 0))
            { ps2_gif_path = 0; PS2_PHASE_END(PS2_PH_GIF); return i + 1; }
    }
    ps2_gif_path = 0;
    PS2_PHASE_END(PS2_PH_GIF);
    return qwords;
}

static u8 *dma_host_ptr(u32 addr) {
    if (addr & 0x80000000u) return ps2_spr + (addr & (PS2_SPR_SIZE - 1u));
    {   u8 *p = ps2_pt[addr >> PS2_PAGE_BITS];
        return p ? p + (addr & PS2_PAGE_MASK) : NULL; }
}

static u32 dma_host_room(u32 addr) {
    if (addr & 0x80000000u)
        return PS2_SPR_SIZE - (addr & (PS2_SPR_SIZE - 1u));
    return PS2_PAGE_SIZE - (addr & PS2_PAGE_MASK);
}

static u32 dma_src_run(u32 addr, u32 want) {
    u32 first, last, p;
    u8 *base;
    if (addr & 0x80000000u) {
        u32 room = PS2_SPR_SIZE - (addr & (PS2_SPR_SIZE - 1u));
        return want < room ? want : room;
    }
    base = ps2_pt[addr >> PS2_PAGE_BITS];
    if (!base) return 0;
    if (!want) return 0;
    first = addr >> PS2_PAGE_BITS;
    {
        u64 end = (u64)addr + (u64)want - 1ull;
        if (end > 0xFFFFFFFFull) end = 0xFFFFFFFFull;
        last = (u32)(end >> PS2_PAGE_BITS);
    }
    if (last >= PS2_PT_ENTRIES) last = PS2_PT_ENTRIES - 1u;
    if (last <= first) return want;
    if (ps2_pt[last] == base + (size_t)(last - first) * PS2_PAGE_SIZE)
        return want;
    for (p = first + 1u; p <= last; p++)
        if (ps2_pt[p] != base + (size_t)(p - first) * PS2_PAGE_SIZE) break;
    return (p << PS2_PAGE_BITS) - addr;
}

static const ps2_reg128 *guest_qw(u32 addr) {
    return (const ps2_reg128 *)dma_host_ptr(addr);
}

static void dma_to_peripheral(int ch, u32 madr, u32 qwc) {
    const ps2_reg128 *src = guest_qw(madr);
    PS2_PHASE_BEGIN(PS2_PH_DMA);
    if (!src) { PS2_PHASE_END(PS2_PH_DMA); return; }
    {
        u32 room = dma_src_run(madr, qwc > 0x0FFFFFFFu ? 0xFFFFFFF0u
                                                       : qwc * 16u) / 16u;
        if (qwc > room) {
            static u32 said;
            if (!said++)
                ps2_log("dma%d: transfer madr=%08X qwc=%u runs past the end of "
                        "mapped memory (%u quadwords available) -- clamped",
                        ch, madr, qwc, room);
            qwc = room;
        }
    }
    if (ch == 1 || ch == 2) rn_dma_transfer(ch, madr, qwc, 0);
    if (g_cap_deep) {
        if (ch == 0 || ch == 1) ps2_cap_vif_qw(ch, src, qwc);
        else if (ch == 2)       ps2_cap_gif_qw(src, qwc);
    }
    switch (ch) {
    case 0: case 1:
        ps2_gfxq_vif_qw(ch, src, qwc);
        if (ch == 1) rn_dma_transfer(ch, madr, qwc, 1);
        break;
    case 2:
        ps2_gfxq_gif_qw(src, qwc);
        rn_dma_transfer(ch, madr, qwc, 1);
        break;
    case 4:
        ps2_ipu_dma_in(src, qwc);
        break;
    case 9: {
        u32 sadr = dma[9].sadr & (PS2_SPR_SIZE - 1u);
        if (ps2_watch_addr && (ps2_watch_addr & 0x80000000u) == 0u) {
            u32 lo = 0x70000000u + sadr, hi = lo + qwc * 16u - 1u;
            if (hi >= ps2_watch_addr && lo <= ps2_watch_last) {
                static u32 said;
                if (said++ < 32u)
                    ps2_log("watch: toSPR wrote %08X..%08X (madr=%08X qwc=%u)",
                            lo, hi, madr, qwc);
            }
        }
        for (u32 i = 0; i < qwc; i++) {
            memcpy(ps2_spr + ((sadr + i * 16) & (PS2_SPR_SIZE - 16)),
                   &src[i], 16);
        }
        dma[9].sadr = sadr + qwc * 16;
        break;
    }
    default:
        break;
    }
    PS2_PHASE_END(PS2_PH_DMA);
}

static void dma_from_peripheral(int ch, u32 madr, u32 qwc) {
    if (ch == 3) {
        u8 *dst = dma_host_ptr(madr);
        if (!dst) return;
        u32 room = dma_src_run(madr, qwc * 16u) / 16u;
        ps2_ipu_dma_out(dst, qwc < room ? qwc : room);
        return;
    }
    if (ch == 8) {
        u8 *dst = dma_host_ptr(madr);
        u32 sadr = dma[8].sadr & (PS2_SPR_SIZE - 1u);
        if (!dst) return;
        for (u32 i = 0; i < qwc; i++)
            memcpy(dst + i * 16,
                   ps2_spr + ((sadr + i * 16) & (PS2_SPR_SIZE - 16)), 16);
        dma[8].sadr = sadr + qwc * 16;
    }
}

static void dma_run_chain(int ch) {
    u32 tte = (dma[ch].chcr >> 6) & 1u;
    u32 tie = (dma[ch].chcr >> 7) & 1u;
    u32 guard = 0;
    while (guard++ < 0x40000) {
        const ps2_reg128 *tagq = guest_qw(dma[ch].tadr);
        u64 tag;
        u32 qwc, id, irq, addr;
        if (!tagq) break;
        tag = tagq->ud[0];
        qwc = (u32)(tag & 0xFFFFu);
        id = (u32)((tag >> 28) & 7u);
        irq = (u32)((tag >> 31) & 1u);
        addr = (u32)((tag >> 32) & 0xFFFFFFF0u);
        if (ch == 0 || ch == 1)
            ps2_vif_note_tag(ch, dma[ch].tadr, (u32)tag, (u32)(tag >> 32));
        if ((ch == 0 || ch == 1 || ch == 2) && qwc > 8192u
            && (id == 0u || id == 1u || id == 2u || id == 7u)) {
            static u32 said;
            if (said++ < 8u) {
                ps2_log("dma%d: implausible DMAtag @%08X -- id=%u qwc=%u "
                        "addr=%08X, raw %08X %08X %08X %08X", ch,
                        dma[ch].tadr, id, qwc, addr,
                        tagq->uw[0], tagq->uw[1], tagq->uw[2], tagq->uw[3]);
                u32 lo = dma[ch].tadr >= 0x40u ? dma[ch].tadr - 0x40u : 0u;
                u32 a;
                ps2_log("   memory around the tag (tag row marked):");
                for (a = lo; a < lo + 0x140u; a += 16u) {
                    const ps2_reg128 *q = guest_qw(a);
                    if (!q) break;
                    ps2_log("   %08X: %08X %08X %08X %08X%s", a,
                            q->uw[0], q->uw[1], q->uw[2], q->uw[3],
                            a == dma[ch].tadr ? "   <-- the tag" : "");
                }
                ps2_dump_trace("implausible DMAtag");
                if (said == 1u) ps2_state_dump_numbered("badtag");
            }
        }
        if ((ch == 1 || ch == 2) && (qwc || (tte && ch == 1)))
            rn_dma_attrib(ch, dma[ch].tadr);
        if (tte && (ch == 0 || ch == 1)) {
            u64 hi = tagq->ud[1];
            if (g_cap_deep) {
                ps2_cap_vif_word(PS2_VIF_WORD_AT(ch, 2), (u32)hi);
                ps2_cap_vif_word(PS2_VIF_WORD_AT(ch, 3), (u32)(hi >> 32));
            }
            ps2_gfxq_vif_word(PS2_VIF_WORD_AT(ch, 2), (u32)hi);
            ps2_gfxq_vif_word(PS2_VIF_WORD_AT(ch, 3), (u32)(hi >> 32));
        }
        switch (id) {
        case 0:
            if (qwc) dma_to_peripheral(ch, addr, qwc);
            dma[ch].tadr += 16;
            guard = 0x40000;
            break;
        case 1:
            if (qwc) dma_to_peripheral(ch, dma[ch].tadr + 16, qwc);
            dma[ch].tadr += 16 + qwc * 16;
            break;
        case 2:
            if (qwc) dma_to_peripheral(ch, dma[ch].tadr + 16, qwc);
            dma[ch].tadr = addr;
            break;
        case 3:
        case 4:
            if (ch == 2 && PS2_ENV("PS2_TRACE_MPEG")) {
                static u32 shown;
                if (shown < 120u) {
                    shown++;
                    ps2_log("gif ref: addr=%08X qwc=%u tadr=%08X",
                            addr, qwc, dma[ch].tadr);
                }
            }
            if (qwc) dma_to_peripheral(ch, addr, qwc);
            dma[ch].tadr += 16;
            break;
        case 5: {
            if (qwc) dma_to_peripheral(ch, dma[ch].tadr + 16, qwc);
            u32 asp = (dma[ch].chcr >> 4) & 3u;
            if (asp >= 2u) { guard = 0x40000; break; }
            u32 ret = dma[ch].tadr + 16 + qwc * 16;
            if (asp == 0) dma[ch].asr0 = ret;
            else dma[ch].asr1 = ret;
            dma[ch].chcr = (dma[ch].chcr & ~0x30u) | ((asp + 1u) << 4);
            dma[ch].tadr = addr;
            break;
        }
        case 6: {
            if (qwc) dma_to_peripheral(ch, dma[ch].tadr + 16, qwc);
            u32 asp = (dma[ch].chcr >> 4) & 3u;
            if (asp == 0 || asp > 2u) { guard = 0x40000; break; }
            if (asp == 2) { dma[ch].tadr = dma[ch].asr1; dma[ch].asr1 = 0; }
            else { dma[ch].tadr = dma[ch].asr0; dma[ch].asr0 = 0; }
            dma[ch].chcr = (dma[ch].chcr & ~0x30u) | ((asp - 1u) << 4);
            break;
        }
        case 7:
            if (qwc) dma_to_peripheral(ch, dma[ch].tadr + 16, qwc);
            guard = 0x40000;
            break;
        default:
            guard = 0x40000;
            break;
        }
        if (irq && tie) break;
        if (guard >= 0x40000) break;
    }
    if (ch == 1 || ch == 2) rn_dma_attrib(ch, 0u);
}

#define IPU_OUT_LOG 64
static struct { u32 madr, n, sum, pos; } ipu_out_log[IPU_OUT_LOG];
static u32 ipu_out_log_n;

static int ipu_tag_end;
static u32 ipu_pull_left;

static void dma_ipu_advance(void) {
    u32 tags = 0;
    while (dma[4].chcr & 0x100u) {
        if (dma[4].qwc) {
            u32 space = ps2_ipu_in_space();
            u32 n, room;
            const ps2_reg128 *src;
            if (space > ipu_pull_left) space = ipu_pull_left;
            if (!space) return;
            n = dma[4].qwc < space ? dma[4].qwc : space;
            room = dma_host_room(dma[4].madr) / 16u;
            if (room && n > room) n = room;
            src = n ? guest_qw(dma[4].madr) : NULL;
            if (!src) { dma[4].qwc = 0; continue; }
            {   static int shown;
                if (shown < 12) {
                    const u8 *b = (const u8 *)src;
                    char hex[3 * 16 + 1];
                    u32 i, k = n * 16u < 16u ? n * 16u : 16u;
                    shown++;
                    for (i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02X ", b[i]);
                    ps2_log("dma4: feed madr=%08X n=%u | %s",
                            dma[4].madr, n, hex);
                }
            }
            ps2_ipu_dma_in(src, n);
            dma[4].madr += n * 16u;
            dma[4].qwc -= n;
            ipu_pull_left -= n;
            continue;
        }
        if (!ipu_tag_end && ((dma[4].chcr >> 2) & 3u) != 1u) {
            static int said;
            if (!said) {
                said = 1;
                ps2_log("dma4: tag drained but mode is %u, not chain -- "
                        "chcr=%08X madr=%08X tadr=%08X: the rest of the chain "
                        "is unreachable", (dma[4].chcr >> 2) & 3u,
                        dma[4].chcr, dma[4].madr, dma[4].tadr);
            }
        }
        if (((dma[4].chcr >> 2) & 3u) != 1u || ipu_tag_end) break;
        if (++tags > 4096u) break;
        {   const ps2_reg128 *tagq = guest_qw(dma[4].tadr);
            u64 tag;
            u32 id, irq, addr;
            if (!tagq) break;
            tag  = tagq->ud[0];
            dma[4].qwc = (u32)(tag & 0xFFFFu);
            id   = (u32)((tag >> 28) & 7u);
            irq  = (u32)((tag >> 31) & 1u);
            addr = (u32)((tag >> 32) & 0xFFFFFFF0u);
            if (!addr && !dma[4].qwc && !id) {
                static int said;
                if (!said) {
                    said = 1;
                    ps2_log("dma4: empty tag at tadr=%08X -- chain already "
                            "ended; stopping instead of feeding from 0",
                            dma[4].tadr);
                }
                ipu_tag_end = 1;
                break;
            }
            switch (id) {
            case 0: dma[4].madr = addr; dma[4].tadr += 16;
                    ipu_tag_end = 1; break;
            case 1: dma[4].madr = dma[4].tadr + 16;
                    dma[4].tadr += 16 + dma[4].qwc * 16; break;
            case 2: dma[4].madr = dma[4].tadr + 16;
                    dma[4].tadr = addr; break;
            case 3: case 4:
                    dma[4].madr = addr; dma[4].tadr += 16; break;
            case 7: dma[4].madr = dma[4].tadr + 16;
                    ipu_tag_end = 1; break;
            default: dma[4].qwc = 0; ipu_tag_end = 1; break;
            }
            if (irq) ipu_tag_end = 1;
        }
    }
    if (!(dma[4].chcr & 0x100u)) return;
    dma[4].chcr &= ~0x100u;
    d_stat |= 1u << 4;
    ipu_tag_end = 0;
}

static void dma_ipu_out_advance(void) {
    while ((dma[3].chcr & 0x100u) && dma[3].qwc) {
        u32 avail = ps2_ipu_out_avail();
        u32 n, room;
        u8 *dst;
        if (!avail) return;
        n = dma[3].qwc < avail ? dma[3].qwc : avail;
        room = dma_host_room(dma[3].madr) / 16u;
        if (room && n > room) n = room;
        dst = dma_host_ptr(dma[3].madr);
        if (!dst || !n) { dma[3].qwc = 0; break; }
        ps2_ipu_dma_out(dst, n);
        if (PS2_ENV("PS2_TRACE_MPEG") && !(dma[3].madr & 0x80000000u)) {
            u32 s = 2166136261u, k;
            u32 slot = ipu_out_log_n++ % IPU_OUT_LOG;
            for (k = 0; k < n * 16u; k++) s = (s ^ dst[k]) * 16777619u;
            ipu_out_log[slot].madr = dma[3].madr;
            ipu_out_log[slot].n = n;
            ipu_out_log[slot].sum = s;
            ipu_out_log[slot].pos = ps2_ipu_out_pos();
        }
        dma[3].madr += n * 16u;
        dma[3].qwc -= n;
    }
    if (!(dma[3].chcr & 0x100u) || dma[3].qwc) return;
    dma[3].chcr &= ~0x100u;
    d_stat |= 1u << 3;
}

void ps2_dmac_ipu_out_history(void) {
    u32 have = ipu_out_log_n < IPU_OUT_LOG ? ipu_out_log_n : IPU_OUT_LOG;
    u32 first = ipu_out_log_n - have, k;
    if (!have) return;
    ps2_log("ipu out: last %u picture writes to main memory, oldest first:",
            have);
    for (k = 0; k < have; k++) {
        u32 i = (first + k) % IPU_OUT_LOG;
        ps2_log("   madr=%08X n=%-5u sum=%08X  out_pos=%u",
                ipu_out_log[i].madr, ipu_out_log[i].n, ipu_out_log[i].sum,
                ipu_out_log[i].pos);
    }
}

void ps2_dmac_ipu_drain(void) { dma_ipu_out_advance(); }

u32 ps2_dmac_pending(void) {
    return d_stat & 0x3FFu;
}

void ps2_dmac_ack(int ch) {
    d_stat &= ~(1u << ch);
}

void ps2_hw_state_save(ps2_state_put put, void *ud) {
    put(ud, "dmac", dma, sizeof dma);
}

void ps2_dmac_ipu_report(u32 *chcr, u32 *madr, u32 *qwc, u32 *tadr) {
    *chcr = dma[4].chcr; *madr = dma[4].madr;
    *qwc  = dma[4].qwc;  *tadr = dma[4].tadr;
}

void ps2_dmac_ipu_history(void) {
    u32 have = ch4_log_n < IPU_CH4_LOG ? ch4_log_n : IPU_CH4_LOG;
    u32 first = ch4_log_n - have, k;
    ps2_log("dma4: last %u CHCR writes before this, oldest first:", have);
    for (k = 0; k < have; k++) {
        u32 i = (first + k) % IPU_CH4_LOG;
        ps2_log("   %s chcr=%08X mod=%u madr=%08X qwc=%u tadr=%08X",
                (ch4_log[i].chcr & 0x100u) ? "START" : "stop ",
                ch4_log[i].chcr, (ch4_log[i].chcr >> 2) & 3u,
                ch4_log[i].madr, ch4_log[i].qwc, ch4_log[i].tadr);
    }
}

void ps2_dmac_ipu_pull(void) {
    u32 want = ps2_ipu_want();
    ipu_pull_left = want > 8u ? want : 8u;
    dma_ipu_advance();
    ipu_pull_left = 0u;
}

static void dma_start(int ch) {
    u32 chcr = dma[ch].chcr;
    u32 mod = (chcr >> 2) & 3u;
    u32 dir = chcr & 1u;
    if (ch == 4) {
        ipu_tag_end = 0;
        ipu_pull_left = 8u;
        dma_ipu_advance();
        ipu_pull_left = 0u;
        return;
    }
    if (ch == 3) {
        u32 want = dma[3].qwc;
        int big = want != 48u;
        static int shown;
        dma_ipu_out_advance();
        if (big && shown < 30) {
            shown++;
            ps2_log("dma3: picture writeback madr=%08X qwc=%u -> %u left, %s",
                    dma[3].madr, want, dma[3].qwc,
                    (dma[3].chcr & 0x100u) ? "STILL RUNNING" : "done");
        }
        return;
    }
    if (ch == 0 || ch == 1) { ps2_cap_vif_clear(ch); ps2_gfxq_vif_clear(ch); }
    {
        int to_peripheral;
        switch (ch) {
        case 0: case 2: case 4: case 6: case 9: to_peripheral = 1; break;
        case 3: case 5: case 8:                 to_peripheral = 0; break;
        default:                                to_peripheral = (int)dir; break;
        }
        if (mod == 1) {
            dma_run_chain(ch);
        } else if (to_peripheral) {
            dma_to_peripheral(ch, dma[ch].madr, dma[ch].qwc);
        } else {
            dma_from_peripheral(ch, dma[ch].madr, dma[ch].qwc);
        }
    }
    dma[ch].qwc = 0;
    dma[ch].chcr &= ~0x100u;
    d_stat |= 1u << ch;
}

static int dma_channel_of(u32 a) {
    switch (a & 0xFFFFFC00u) {
    case 0x10008000u: return 0;
    case 0x10009000u: return 1;
    case 0x1000A000u: return 2;
    case 0x1000B000u: return 3;
    case 0x1000B400u: return 4;
    case 0x1000C000u: return 5;
    case 0x1000C400u: return 6;
    case 0x1000C800u: return 7;
    case 0x1000D000u: return 8;
    case 0x1000D400u: return 9;
    default: return -1;
    }
}

static u32 hw_read32(u32 a) {
    int ch;
    switch (a) {
    case 0x10000000u: case 0x10000800u: case 0x10001000u: case 0x10001800u: {
        int t = (int)((a >> 11) & 3u);
        timer_sync(t);
        timer_reads++;
        return timer_count[t];
    }
    case 0x10000010u: case 0x10000810u: case 0x10001010u: case 0x10001810u: {
        int t = (int)((a >> 11) & 3u);
        timer_sync(t);
        return timer_mode[t];
    }
    case 0x10000020u: return timer_comp[0];
    case 0x10000030u: return timer_hold[0];
    case 0x10000820u: return timer_comp[1];
    case 0x10000830u: return timer_hold[1];
    case 0x10001020u: return timer_comp[2];
    case 0x10001820u: return timer_comp[3];
    case 0x10002000u: case 0x10002004u:
    case 0x10002010u: case 0x10002014u:
    case 0x10002020u: case 0x10002024u:
    case 0x10002030u: case 0x10002034u:
        return ps2_ipu_read32(a);
    case 0x10003000u: return gif_ctrl;
    case 0x10003010u: return gif_mode;
    case 0x10003020u: return gif_stat;
    case 0x1000E000u: return d_ctrl;
    case 0x1000E010u: return d_stat;
    case 0x1000E020u: return d_pcr;
    case 0x1000E030u: return d_sqwc;
    case 0x1000E040u: return d_rbsr;
    case 0x1000E050u: return d_rbor;
    case 0x1000E060u: return d_stadr;
    case 0x1000F000u: ps2_kernel_poll_vblank(); return intc_stat;
    case 0x1000F010u: return intc_mask;
    case 0x1000F520u: return d_enable;
    default: break;
    }
    if (a >= 0x1000F200u && a < 0x1000F300u) return ps2_sif_reg_read(a);
    ch = dma_channel_of(a);
    if (ch >= 0) {
        switch (a & 0xFFu) {
        case 0x00: return dma[ch].chcr;
        case 0x10: return dma[ch].madr;
        case 0x20: return dma[ch].qwc;
        case 0x30: return dma[ch].tadr;
        case 0x40: return dma[ch].asr0;
        case 0x50: return dma[ch].asr1;
        case 0x80: return dma[ch].sadr;
        default: return 0;
        }
    }
    return 0;
}

static void hw_write32(u32 a, u32 v) {
    int ch;
    switch (a) {
    case 0x10000000u: case 0x10000800u: case 0x10001000u: case 0x10001800u: {
        int t = (int)((a >> 11) & 3u);
        timer_sync(t);
        timer_count[t] = v & 0xFFFFu;
        timer_frac[t] = 0;
        return;
    }
    case 0x10000010u: case 0x10000810u: case 0x10001010u: case 0x10001810u:
        timer_write_mode((int)((a >> 11) & 3u), v);
        return;
    case 0x10000020u: timer_sync(0); timer_comp[0] = v & 0xFFFFu; return;
    case 0x10000030u: timer_hold[0] = v; return;
    case 0x10000820u: timer_sync(1); timer_comp[1] = v & 0xFFFFu; return;
    case 0x10000830u: timer_hold[1] = v; return;
    case 0x10001020u: timer_sync(2); timer_comp[2] = v & 0xFFFFu; return;
    case 0x10001820u: timer_sync(3); timer_comp[3] = v & 0xFFFFu; return;
    case 0x10002000u: case 0x10002010u:
    case 0x10002020u: case 0x10002030u:
        ps2_ipu_write32(a, v); return;
    case 0x10003000u: ps2_cap_mmio(a, v); ps2_gfxq_mmio(a, v); return;
    case 0x10003010u: ps2_cap_mmio(a, v); ps2_gfxq_mmio(a, v); return;
    case 0x1000E000u: d_ctrl = v; return;
    case 0x1000E010u: d_stat = (d_stat & ~(v & 0xFFFFu)) ^ (v & 0xFFFF0000u); return;
    case 0x1000E020u: d_pcr = v; return;
    case 0x1000E030u: d_sqwc = v; return;
    case 0x1000E040u: d_rbsr = v; return;
    case 0x1000E050u: d_rbor = v; return;
    case 0x1000E060u: d_stadr = v; return;
    case 0x1000F000u: intc_stat &= ~v; return;
    case 0x1000F010u: intc_mask ^= v; return;
    case 0x1000F520u: d_enable = v; return;
    case 0x1000F590u: d_enable = v; return;
    default: break;
    }
    if (a >= 0x1000F200u && a < 0x1000F300u) { ps2_sif_reg_write(a, v); return; }
    if (a == 0x10003810u) {
        ps2_cap_vif_reset(0, v); ps2_gfxq_fbrst(0, v); return; }
    if (a == 0x10003C10u) {
        ps2_cap_vif_reset(1, v); ps2_gfxq_fbrst(1, v); return; }
    if (a >= 0x10003800u && a < 0x10003C00u) {
        ps2_cap_vif_word(0, v); ps2_gfxq_vif_word(0, v); return; }
    if (a >= 0x10003C00u && a < 0x10004000u) {
        ps2_cap_vif_word(1, v); ps2_gfxq_vif_word(1, v); return; }
    ch = dma_channel_of(a);
    if (ch >= 0) {
        switch (a & 0xFFu) {
        case 0x00:
            if (dma[ch].chcr & 0x100u)
                dma[ch].chcr = (dma[ch].chcr & ~0x100u) | (v & 0x100u);
            else
                dma[ch].chcr = v;
            v = dma[ch].chcr;
            if (ch == 4) ch4_log_push(v);
            if (ch == 4) {
                static unsigned seen;
                if (seen < 12u) {
                    seen++;
                    ps2_log("dma4: CHCR write #%u = %08X  mod=%u str=%u "
                            "madr=%08X qwc=%u tadr=%08X", seen, v,
                            (v >> 2) & 3u, (v >> 8) & 1u,
                            dma[4].madr, dma[4].qwc, dma[4].tadr);
                }
            }
            if (v & 0x100u) dma_start(ch);
            return;
        case 0x10: dma[ch].madr = v; return;
        case 0x20: dma[ch].qwc = v; return;
        case 0x30: dma[ch].tadr = v; return;
        case 0x40: dma[ch].asr0 = v; return;
        case 0x50: dma[ch].asr1 = v; return;
        case 0x80: dma[ch].sadr = v; return;
        default: return;
        }
    }
}

u8 ps2_mmio_r8(u32 a) { return (u8)(hw_read32(a & ~3u) >> ((a & 3u) * 8)); }
u16 ps2_mmio_r16(u32 a) { return (u16)(hw_read32(a & ~3u) >> ((a & 2u) * 8)); }

u32 ps2_mmio_r32(u32 a) {
    if (a >= 0x12000000u && a < 0x12002000u) return (u32)ps2_gs_priv_read(a);
    return hw_read32(a);
}

u64 ps2_mmio_r64(u32 a) {
    if (a >= 0x12000000u && a < 0x12002000u) return ps2_gs_priv_read(a);
    return (u64)hw_read32(a) | ((u64)hw_read32(a + 4) << 32);
}

void ps2_mmio_r128(ps2_reg128 *d, u32 a) {
    if (a >= 0x10007000u && a < 0x10007010u) { ps2_ipu_dma_out(d, 1); return; }
    d->ud[0] = ps2_mmio_r64(a);
    d->ud[1] = ps2_mmio_r64(a + 8);
}

void ps2_mmio_w8(u32 a, u8 v) {
    u32 sh = (a & 3u) * 8;
    u32 w = hw_read32(a & ~3u);
    hw_write32(a & ~3u, (w & ~(0xFFu << sh)) | ((u32)v << sh));
}

void ps2_mmio_w16(u32 a, u16 v) {
    u32 sh = (a & 2u) * 8;
    u32 w = hw_read32(a & ~3u);
    hw_write32(a & ~3u, (w & ~(0xFFFFu << sh)) | ((u32)v << sh));
}

void ps2_mmio_w32(u32 a, u32 v) {
    if (a >= 0x12000000u && a < 0x12002000u) { ps2_gs_priv_write(a, v); return; }
    hw_write32(a, v);
}

void ps2_mmio_w64(u32 a, u64 v) {
    if (a >= 0x12000000u && a < 0x12002000u) { ps2_gs_priv_write(a, v); return; }
    hw_write32(a, (u32)v);
    hw_write32(a + 4, (u32)(v >> 32));
}

void ps2_mmio_w128(u32 a, const ps2_reg128 *v) {
    if (a >= 0x10006000u && a < 0x10007000u) {
        ps2_cap_gif_qw(v, 1); ps2_gfxq_gif_qw(v, 1); return; }
    if (a >= 0x10004000u && a < 0x10005000u) {
        ps2_cap_vif_qw(0, v, 1); ps2_gfxq_vif_qw(0, v, 1); return; }
    if (a >= 0x10005000u && a < 0x10006000u) {
        ps2_cap_vif_qw(1, v, 1); ps2_gfxq_vif_qw(1, v, 1); return; }
    if (a >= 0x10007010u && a < 0x10007020u) { ps2_ipu_dma_in(v, 1); return; }
    ps2_mmio_w64(a, v->ud[0]);
    ps2_mmio_w64(a + 8, v->ud[1]);
}

static u64 gs_priv[0x2000 / 16];
static u64 gs_priv_ee[0x2000 / 16];

void ps2_gs_priv_cap_save(u64 *priv, u64 *csr, u64 *imr) {
    ps2_gfxq_drain();
    memcpy(priv, gs_priv, sizeof gs_priv);
    *csr = gs_csr; *imr = gs_imr;
}
void ps2_gs_priv_cap_load(const u64 *priv, u64 csr, u64 imr) {
    ps2_gfxq_drain();
    memcpy(gs_priv, priv, sizeof gs_priv);
    memcpy(gs_priv_ee, priv, sizeof gs_priv_ee);
    gs_csr = csr; gs_imr = imr;
}

static u64 dispfb_writes, dispfb_flips;
double ps2_wall_seconds(void);

extern u64 gs_guest_frames;

void ps2_gs_display_report(void) {
    double secs = ps2_wall_seconds();
    if (!dispfb_writes && !gs_guest_frames) return;
    ps2_log("display: %llu displayed-target transitions = %.1f/s over %.1f s "
            "(not a simulation-speed measurement). DISPFB written "
            "%llu times, %llu of them a change of buffer.",
            (unsigned long long)gs_guest_frames,
            secs > 0.0 ? (double)gs_guest_frames / secs : 0.0, secs,
            (unsigned long long)dispfb_writes,
            (unsigned long long)dispfb_flips);
}

void ps2_gs_priv_write_now(u32 addr, u64 val) {
    u32 idx = (addr & 0x1FF0u) >> 4;
    if (addr == 0x12000070u || addr == 0x12000090u) {
        dispfb_writes++;
        if (gs_priv[idx & 0x1FFu] != val) dispfb_flips++;
    }
    if (ps2_verbose && (addr == 0x12000070u || addr == 0x12000090u
                        || addr == 0x12000080u || addr == 0x120000A0u
                        || addr == 0x12000000u || addr == 0x12000010u))
        ps2_log("gs: priv %08X <- %016llX", addr, (unsigned long long)val);
    gs_priv[idx & 0x1FFu] = val;
}

void ps2_gif_ctrl_write(u32 addr, u32 val) {
    if (addr == 0x10003000u) gif_ctrl = val;
    else                     gif_mode = val;
}

void ps2_gs_priv_write(u32 addr, u64 val) {
    ps2_cap_priv(addr, val);
    if (addr == 0x12001000u) {
        gs_csr &= ~(val & 0x1Full);
        if (val & GS_CSR_RESET) gs_csr = 0;
        return;
    }
    if (addr == 0x12001010u) { gs_imr = val; return; }
    gs_priv_ee[((addr & 0x1FF0u) >> 4) & 0x1FFu] = val;
    ps2_gfxq_gs_priv(addr, val);
}

u64 ps2_gs_priv_read(u32 addr) {
    u32 idx = (addr & 0x1FF0u) >> 4;
    if (addr == 0x12001000u) return gs_csr;
    if (addr == 0x12001010u) return gs_imr;
    return gs_priv_ee[idx & 0x1FFu];
}

void ps2_gs_vblank(void) {
    gs_csr ^= GS_CSR_FIELD;
    gs_csr |= GS_CSR_VSINT;
    ps2_intc_raise(PS2_INTC_VBLANK_S);
    ps2_intc_raise(PS2_INTC_VBLANK_E);
}

u32 ps2_gs_field_parity(void) { return (gs_csr & GS_CSR_FIELD) ? 1u : 0u; }
void ps2_gs_set_field_parity(u32 odd) {
    if (odd) gs_csr |= GS_CSR_FIELD;
    else     gs_csr &= ~GS_CSR_FIELD;
}
