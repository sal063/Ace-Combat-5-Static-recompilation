#include "ps2_runtime.h"
#include "ps2_settings.h"
#include "ps2_hle.h"
#include "ps2_capture.h"
#include "ps2_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

void ps2_gs_init(void);
void ps2_gs_write_reg(u32 reg, u64 val);
void ps2_gs_priv_write(u32 addr, u64 val);
int  ps2_gs_present(void);
void ps2_vu1_init(void);
void ps2_gif_transfer(const ps2_reg128 *data, u32 qwc);
void ps2_vif_fifo(int which, const ps2_reg128 *q);
void ps2_vif_write(int which, u32 word);
void ps2_vif_fbrst(int which, u32 val);
void ps2_vif_clear_stall(int which);
int  ps2_vif_stalled(int which);
void ps2_gs_stats(u64 *prims, u64 *pixels, u64 *regs);
void ps2_gs_trx_stats(u64 *transfers, u64 *pixels);
void ps2_mmio_w32(u32 a, u32 v);
void ps2_vu_perf(u64 *insns, u64 *runs);
void ps2_gs_tex_cache_stats(u64 *hits, u64 *misses, u64 *texels, u64 *scan);
void ps2_gs_shadow_report(void);
const u8 *ps2_gs_framebuffer(u32 *w, u32 *h, u32 *stride);
void ps2_gs_tex_census_report(void);
void ps2_vu_profile_report(void);
void ps2_gs_draw_census_report(void);
void ps2_gs_texovl_report(void);
void ps2_gs_fill_report(void);
void ps2_gs_trx_census_report(void);

#define RB_CAP (8u << 20)
static FILE  *rf;
static u8    *rbuf;
static size_t rb_n, rb_p;
static u8    *rb_scratch_raw, *rb_scratch;
static size_t rb_scratch_n;
static u64    rb_consumed;

static int rb_fill(size_t need) {
    if (rb_n - rb_p >= need) return 1;
    memmove(rbuf, rbuf + rb_p, rb_n - rb_p);
    rb_n -= rb_p;
    rb_p = 0;
    while (rb_n < need) {
        size_t got = fread(rbuf + rb_n, 1, RB_CAP - rb_n, rf);
        if (!got) return 0;
        rb_n += got;
    }
    return 1;
}

static const u8 *rb_take(size_t n) {
    const u8 *p;
    if (!rb_fill(n)) return NULL;
    p = rbuf + rb_p;
    rb_p += n;
    rb_consumed += n;
    return p;
}

static const u8 *rb_payload(size_t n) {
    size_t have;
    if (n > rb_scratch_n) {
        u8 *p = (u8 *)realloc(rb_scratch_raw, n + 16u);
        if (!p) return NULL;
        rb_scratch_raw = p;
        rb_scratch = (u8 *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
        rb_scratch_n = n;
    }
    have = rb_n - rb_p;
    if (have > n) have = n;
    memcpy(rb_scratch, rbuf + rb_p, have);
    rb_p += have;
    if (have < n && fread(rb_scratch + have, 1, n - have, rf) != n - have)
        return NULL;
    rb_consumed += n;
    return rb_scratch;
}

static u8  rd_u8(int *ok)  { const u8 *p = rb_take(1); if (!p) { *ok = 0; return 0; } return *p; }
static u32 rd_u32(int *ok) { const u8 *p = rb_take(4); u32 v; if (!p) { *ok = 0; return 0; } memcpy(&v, p, 4); return v; }
static u64 rd_u64(int *ok) { const u8 *p = rb_take(8); u64 v; if (!p) { *ok = 0; return 0; } memcpy(&v, p, 8); return v; }

static ps2_cap_hdr       hdr;
static ps2_gs_capstate   gst;
static ps2_vu_capstate   vst;
static ps2_gif_capstate  fst;
static u8 *snap_vram, *snap_spr;
static u8 *snap_vu1_mem, *snap_vu1_micro, *snap_vu0_mem, *snap_vu0_micro;

static u8 *slurp(FILE *f, u32 n) {
    u8 *p;
    if (!n) return NULL;
    p = (u8 *)malloc(n);
    if (!p) { fprintf(stderr, "[REPLAY] out of memory (%u bytes)\n", n); exit(1); }
    if (fread(p, 1, n, f) != n) {
        fprintf(stderr, "[REPLAY] short read of a %u-byte snapshot block\n", n);
        exit(1);
    }
    return p;
}

static void copy_into(u8 *dst, u32 dst_n, const u8 *src, u32 src_n) {
    if (!dst || !src) return;
    memcpy(dst, src, src_n < dst_n ? src_n : dst_n);
}

static void restore_snapshot(void) {
    u32 n;
    u8 *p;
    p = ps2_gs_vram_ptr(&n);
    copy_into(p, n, snap_vram, hdr.vram_size);
    if (ps2_spr) copy_into(ps2_spr, PS2_SPR_SIZE, snap_spr, hdr.spr_size);
    p = ps2_vu_cap_mem(1, &n);   copy_into(p, n, snap_vu1_mem,   hdr.vu1_mem_size);
    p = ps2_vu_cap_micro(1, &n); copy_into(p, n, snap_vu1_micro, hdr.vu1_micro_size);
    p = ps2_vu_cap_mem(0, &n);   copy_into(p, n, snap_vu0_mem,   hdr.vu0_mem_size);
    p = ps2_vu_cap_micro(0, &n); copy_into(p, n, snap_vu0_micro, hdr.vu0_micro_size);
    ps2_vu_cap_load(&vst);
    ps2_gif_cap_load(&fst);
    ps2_gs_cap_load(&gst);
    ps2_cap_dict_reset();
}

typedef struct {
    u64 fields, records;
    u64 regs, prims, trx;
    u64 vu_insns, vu_runs;
    u64 tex_hits, tex_misses, tex_texels, tex_scan;
    u64 exp_regs, exp_prims, exp_trx;
    u32 mismatch_fields;
    u32 first_mismatch;
    int closed;
} pass_result;

static int quiet = 1;

static void replay_pass(u32 field_limit, pass_result *res) {
    u64 base_prims, base_pixels, base_regs, base_trx, base_tpix;
    u64 base_vi, base_vr;
    u64 base_th, base_tm, base_tt, base_ts;
    u64 last_regs, last_prims, last_trx;
    u64 exp_r = 0, exp_p = 0, exp_t = 0;
    int have_exp = 0;
    int ok = 1;
    u64 stream_end = hdr.stream_bytes;

    if (!stream_end && hdr.index_off > hdr.stream_off)
        stream_end = hdr.index_off - hdr.stream_off;

    memset(res, 0, sizeof *res);
    res->first_mismatch = 0xFFFFFFFFu;
    ps2_gs_stats(&base_prims, &base_pixels, &base_regs);
    ps2_gs_trx_stats(&base_trx, &base_tpix);
    ps2_vu_perf(&base_vi, &base_vr);
    ps2_gs_tex_cache_stats(&base_th, &base_tm, &base_tt, &base_ts);
    last_regs = base_regs; last_prims = base_prims; last_trx = base_trx;

    fseek(rf, (long)hdr.stream_off, SEEK_SET);
    rb_n = rb_p = 0;
    rb_consumed = 0;

    while (ok && (!stream_end || rb_consumed < stream_end)) {
        u8 op = rd_u8(&ok);
        if (!ok) break;
        res->records++;
        switch (op) {
        case PS2_CAP_OP_REG: {
            u32 r = rd_u8(&ok);
            u64 v = rd_u64(&ok);
            if (ok) ps2_gs_write_reg(r, v);
            break;
        }
        case PS2_CAP_OP_HWRUN: {
            u32 n = rd_u32(&ok), i;
            for (i = 0; ok && i < n; i++) {
                u64 v = rd_u64(&ok);
                if (ok) ps2_gs_write_reg(0x54u, v);
            }
            break;
        }
        case PS2_CAP_OP_PRIV: {
            u32 a = rd_u32(&ok);
            u64 v = rd_u64(&ok);
            if (ok) ps2_gs_priv_write(a, v);
            break;
        }
        case PS2_CAP_OP_VIFW: {
            u32 ch = rd_u8(&ok);
            u32 w = rd_u32(&ok);
            if (ok) ps2_vif_write((int)ch, w);
            break;
        }
        case PS2_CAP_OP_FBRST: {
            u32 ch = rd_u8(&ok);
            u32 v = rd_u32(&ok);
            if (ok) ps2_vif_fbrst((int)(ch & 1u), v);
            break;
        }
        case PS2_CAP_OP_VIFCLR: {
            u32 ch = rd_u8(&ok);
            if (ok) ps2_vif_clear_stall((int)(ch & 1u));
            break;
        }
        case PS2_CAP_OP_MMIO: {
            u32 a = rd_u32(&ok);
            u32 v = rd_u32(&ok);
            if (ok) ps2_mmio_w32(a, v);
            break;
        }
        case PS2_CAP_OP_VIFQW:
        case PS2_CAP_OP_GIFQW: {
            int is_vif = op == PS2_CAP_OP_VIFQW;
            u32 ch = is_vif ? (u32)rd_u8(&ok) : 0u;
            u32 qwc = rd_u32(&ok);
            const u8 *p;
            if (!ok || !qwc) break;
            p = rb_payload((size_t)qwc * 16u);
            if (!p) { ok = 0; break; }
            ps2_cap_dict_insert(p, qwc * 16u);
            if (is_vif) {
                u32 i;
                if (!ps2_vif_stalled((int)(ch & 1u)))
                    for (i = 0; i < qwc; i++) {
                        ps2_vif_fifo((int)(ch & 1u), &((const ps2_reg128 *)p)[i]);
                        if (ps2_vif_stalled((int)(ch & 1u))) break;
                    }
            } else {
                ps2_gif_transfer((const ps2_reg128 *)p, qwc);
            }
            break;
        }
        case PS2_CAP_OP_VIFREF:
        case PS2_CAP_OP_GIFREF: {
            int is_vif = op == PS2_CAP_OP_VIFREF;
            u32 ch = is_vif ? (u32)rd_u8(&ok) : 0u;
            u32 slot = rd_u32(&ok);
            u32 len = 0;
            const u8 *p;
            if (!ok) break;
            p = ps2_cap_dict_get(slot, &len);
            if (!p || !len) {
                fprintf(stderr, "[REPLAY] dictionary slot %u is empty at "
                                "record %llu -- the capture and this build "
                                "disagree about the dictionary rules\n",
                        slot, (unsigned long long)res->records);
                ok = 0;
                break;
            }
            if (is_vif) {
                u32 i, qwc = len / 16u;
                if (!ps2_vif_stalled((int)(ch & 1u)))
                    for (i = 0; i < qwc; i++) {
                        ps2_vif_fifo((int)(ch & 1u), &((const ps2_reg128 *)p)[i]);
                        if (ps2_vif_stalled((int)(ch & 1u))) break;
                    }
            } else {
                ps2_gif_transfer((const ps2_reg128 *)p, len / 16u);
            }
            break;
        }
        case PS2_CAP_OP_STATS:
            exp_r = rd_u64(&ok);
            exp_p = rd_u64(&ok);
            exp_t = rd_u64(&ok);
            have_exp = ok;
            break;
        case PS2_CAP_OP_FIELD: {
            u32 field = rd_u32(&ok);
            u32 parity = rd_u32(&ok);
            u64 prims, pixels, regs, trx, tpix;
            if (!ok) break;
            ps2_gs_set_field_parity(parity);
            if (!ps2_gs_present()) { res->closed = 1; ok = 0; break; }
            res->fields++;
            ps2_gs_stats(&prims, &pixels, &regs);
            ps2_gs_trx_stats(&trx, &tpix);
            if (have_exp) {
                u64 dr = regs - last_regs, dp = prims - last_prims,
                    dt = trx - last_trx;
                res->exp_regs += exp_r; res->exp_prims += exp_p;
                res->exp_trx += exp_t;
                if (dr != exp_r || dp != exp_p || dt != exp_t) {
                    if (res->mismatch_fields == 0 || !quiet)
                        fprintf(stderr, "[REPLAY] field %u differs from the "
                                "recorded run: GS regs %llu vs %llu, prims "
                                "%llu vs %llu, transfers %llu vs %llu\n",
                                field,
                                (unsigned long long)dr, (unsigned long long)exp_r,
                                (unsigned long long)dp, (unsigned long long)exp_p,
                                (unsigned long long)dt, (unsigned long long)exp_t);
                    if (res->mismatch_fields == 0) res->first_mismatch = field;
                    res->mismatch_fields++;
                }
            }
            last_regs = regs; last_prims = prims; last_trx = trx;
            have_exp = 0;
            if (field_limit && res->fields >= field_limit) ok = 0;
            break;
        }
        case PS2_CAP_OP_END:
            ok = 0;
            break;
        default:
            fprintf(stderr, "[REPLAY] bad record tag %u at record %llu\n",
                    (unsigned)op, (unsigned long long)res->records);
            ok = 0;
            break;
        }
    }
    {
        u64 prims, pixels, regs, trx, tpix, vi, vr, th_, tm, tt, ts;
        ps2_gs_stats(&prims, &pixels, &regs);
        ps2_gs_trx_stats(&trx, &tpix);
        ps2_vu_perf(&vi, &vr);
        ps2_gs_tex_cache_stats(&th_, &tm, &tt, &ts);
        res->tex_hits = th_ - base_th;
        res->tex_misses = tm - base_tm;
        res->tex_texels = tt - base_tt;
        res->tex_scan = ts - base_ts;
        res->regs = regs - base_regs;
        res->prims = prims - base_prims;
        res->trx = trx - base_trx;
        res->vu_insns = vi - base_vi;
        res->vu_runs = vr - base_vr;
    }
}

static void usage(const char *argv0) {
    printf(
"usage: %s [capture.gscap] [options]\n"
"  --loop N        replay N times, 0 = until the window is closed\n"
"  --fields N      stop each pass after N fields\n"
"  --bench         time every pass and print the phase profile\n"
"  --novideo       run headless -- but NOT the fast path and NOT what you\n"
"                  want for a measurement: with no Vulkan device, vertex_kick\n"
"                  falls through to the software rasteriser, which is a\n"
"                  different code path and about nine times slower\n"
"  --shot FILE     write the last frame as a PPM (default ps2_replay.ppm)\n"
"  --verbose       the runtime's own tracing\n"
"  --loud          report every field that differs from the recorded run\n"
"  --census        arm the per-primitive censuses and draw traces.  They are\n"
"                  OFF by default here: they are armed at startup and the\n"
"                  game disarms them on a path a replay never reaches, so\n"
"                  leaving them on measures the diagnostics, not the work.\n"
"\n"
"A capture is taken from the game with F6, or with --capture on ac5.exe.\n"
"Set PS2_PROFILE_PHASES=1 for the per-phase breakdown a --bench pass prints,\n"
"and PS2_PRESENT_MODE=immediate so the swapchain does not pace the benchmark.\n",
    argv0);
}

int main(int argc, char **argv) {
    const char *path = "out/capture.gscap";
    const char *shot = "ps2_replay.ppm";
    int want_video = 1, bench = 0, loops = 1, census = 0;
    u32 field_limit = 0;
    double t_total = 0.0;
    u64 fields_total = 0;
    int pass;

    ps2_settings_disable_file();
    ps2_settings_load();
    g_cap_disable = 1;
    ps2_wall_seconds();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--novideo")) want_video = 0;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) ps2_verbose = 1;
        else if (!strcmp(argv[i], "--loud")) quiet = 0;
        else if (!strcmp(argv[i], "--census")) census = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--loop") && i + 1 < argc)
            loops = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fields") && i + 1 < argc)
            field_limit = (u32)strtoul(argv[++i], NULL, 0);
        else if (argv[i][0] != '-') path = argv[i];
        else { usage(argv[0]); return strcmp(argv[i], "--help") ? 1 : 0; }
    }

    { const char *e = getenv("PS2_CENSUS");
      ps2_diag_armed = census || (e && atoi(e) != 0); }

    rf = fopen(path, "rb");
    if (!rf) { fprintf(stderr, "[REPLAY] cannot open %s\n", path); return 1; }
    if (fread(&hdr, sizeof hdr, 1, rf) != 1
        || memcmp(hdr.magic, PS2_CAP_MAGIC, 6)) {
        fprintf(stderr, "[REPLAY] %s: not a %s capture.  Captures written by "
                        "an older build are not readable; take a new one.\n",
                path, PS2_CAP_MAGIC);
        return 1;
    }
    if (hdr.version != PS2_CAP_VERSION) {
        fprintf(stderr, "[REPLAY] capture version %u, this build reads %u\n",
                hdr.version, PS2_CAP_VERSION);
        return 1;
    }
    if (hdr.gs_state_size != (u32)sizeof(ps2_gs_capstate)
        || hdr.vu_state_size != (u32)sizeof(ps2_vu_capstate)
        || hdr.gif_state_size != (u32)sizeof(ps2_gif_capstate)) {
        fprintf(stderr, "[REPLAY] state size mismatch (capture %u/%u/%u, "
                        "build %u/%u/%u) -- recapture with this build\n",
                hdr.gs_state_size, hdr.vu_state_size, hdr.gif_state_size,
                (u32)sizeof(ps2_gs_capstate), (u32)sizeof(ps2_vu_capstate),
                (u32)sizeof(ps2_gif_capstate));
        return 1;
    }

    if (fread(&gst, sizeof gst, 1, rf) != 1
        || fread(&vst, sizeof vst, 1, rf) != 1) {
        fprintf(stderr, "[REPLAY] short GS/VU state\n"); return 1;
    }
    snap_vu1_mem   = slurp(rf, hdr.vu1_mem_size);
    snap_vu1_micro = slurp(rf, hdr.vu1_micro_size);
    snap_vu0_mem   = slurp(rf, hdr.vu0_mem_size);
    snap_vu0_micro = slurp(rf, hdr.vu0_micro_size);
    if (fread(&fst, sizeof fst, 1, rf) != 1) {
        fprintf(stderr, "[REPLAY] short GIF state\n"); return 1;
    }
    snap_vram = slurp(rf, hdr.vram_size);
    snap_spr  = slurp(rf, hdr.spr_size);

    ps2_mem_init();
    ps2_gs_init();
    ps2_vu1_init();

    if (hdr.nblk) {
        u32 bmbytes = (hdr.nblk + 7u) / 8u, b, nz = 0;
        u8 *bm = (u8 *)malloc(bmbytes);
        if (!bm || fread(bm, 1, bmbytes, rf) != bmbytes) {
            fprintf(stderr, "[REPLAY] short block bitmap\n"); return 1;
        }
        for (b = 0; b < hdr.nblk; b++)
            if (bm[b >> 3] & (1u << (b & 7u))) {
                if (fread(ps2_ram + ((u64)b << hdr.blk_shift), 1,
                          (size_t)1 << hdr.blk_shift, rf)
                    != ((size_t)1 << hdr.blk_shift)) {
                    fprintf(stderr, "[REPLAY] short memory block %u\n", b);
                    return 1;
                }
                nz++;
            }
        free(bm);
        fprintf(stderr, "[REPLAY] %u/%u guest RAM blocks restored\n",
                nz, hdr.nblk);
    }

    ps2_cap_dict_config(hdr.dict_slots, hdr.dict_min, hdr.dict_max,
                        hdr.dict_budget);

    {   const char *e = getenv("PS2_LOCKSTEP");
        ps2_vk_lockstep = (e && *e) ? (*e != '0') : 1;
    }
    if (want_video && ps2_video_init("gsreplay", 640, 448) != 0) {
        fprintf(stderr, "[REPLAY] no video; continuing headless\n");
        want_video = 0;
    }

    fprintf(stderr, "[REPLAY] %s: level %s, %u fields, %llu records, "
                    "%.1f MB of stream, %u KB VRAM%s\n",
            path, hdr.level == PS2_CAP_LEVEL_DMA ? "dma" : "gs",
            hdr.nfields, (unsigned long long)hdr.nrec,
            (double)hdr.stream_bytes / (1024.0 * 1024.0),
            hdr.vram_size / 1024u, hdr.truncated ? "  (TRUNCATED)" : "");
    if (hdr.live_us_per_field)
        fprintf(stderr, "[REPLAY] the game itself delivered these fields at "
                "%.1f/s (%.2f ms/field) -- 59.94/s is full speed\n",
                1e6 / (double)hdr.live_us_per_field,
                (double)hdr.live_us_per_field / 1000.0);

    rbuf = (u8 *)malloc(RB_CAP);
    if (!rbuf) { fprintf(stderr, "[REPLAY] out of memory\n"); return 1; }

    for (pass = 0; loops == 0 || pass < loops; pass++) {
        pass_result r;
        double t0, dt;
        restore_snapshot();
        if (bench) ps2_phase_init();
        t0 = ps2_wall_seconds();
        replay_pass(field_limit, &r);
        dt = ps2_wall_seconds() - t0;
        t_total += dt;
        fields_total += r.fields;
        fprintf(stderr,
                "[REPLAY] pass %d: %llu fields in %.2f s = %.1f fields/s "
                "(%.2f ms/field) | %llu prims, %llu GS regs, %llu transfers\n",
                pass + 1, (unsigned long long)r.fields, dt,
                r.fields ? (double)r.fields / dt : 0.0,
                r.fields ? dt * 1000.0 / (double)r.fields : 0.0,
                (unsigned long long)r.prims, (unsigned long long)r.regs,
                (unsigned long long)r.trx);
        if (r.vu_insns && r.fields)
            fprintf(stderr, "[REPLAY]         VU: %llu instructions "
                    "(%.0f/field) in %llu activations\n",
                    (unsigned long long)r.vu_insns,
                    (double)r.vu_insns / (double)r.fields,
                    (unsigned long long)r.vu_runs);
        if ((r.tex_hits || r.tex_misses) && r.fields)
            fprintf(stderr, "[REPLAY]        tex: %llu hits / %llu misses "
                    "(%.1f%% hit), %.0f texels/field decoded, "
                    "%.1f entries scanned per lookup\n",
                    (unsigned long long)r.tex_hits,
                    (unsigned long long)r.tex_misses,
                    100.0 * (double)r.tex_hits
                          / (double)(r.tex_hits + r.tex_misses),
                    (double)r.tex_texels / (double)r.fields,
                    (double)r.tex_scan
                        / (double)(r.tex_hits + r.tex_misses));
        if (r.mismatch_fields)
            fprintf(stderr,
                    "[REPLAY] FIDELITY: %u of %llu fields do not match the "
                    "recorded run (first at field %u).  The replay is drawing "
                    "something the game did not.\n",
                    r.mismatch_fields, (unsigned long long)r.fields,
                    r.first_mismatch);
        else if (r.fields)
            fprintf(stderr, "[REPLAY] fidelity: every field matches the "
                            "recorded run exactly\n");
        if (bench) { ps2_phase_report(r.fields); ps2_gs_shadow_report(); }
        ps2_vu_profile_report();
        if (census) {
            ps2_gs_trx_census_report();
            ps2_gs_tex_census_report();
            ps2_gs_texovl_report();
            ps2_gs_fill_report();
            ps2_gs_draw_census_report();
        }
        if (r.closed) { fprintf(stderr, "[REPLAY] window closed\n"); break; }
        if (want_video && ps2_vk_closed()) break;
    }

    if (fields_total)
        fprintf(stderr, "[REPLAY] total: %llu fields in %.2f s = "
                        "%.1f fields/s (%.2f ms/field)\n",
                (unsigned long long)fields_total, t_total,
                (double)fields_total / t_total,
                t_total * 1000.0 / (double)fields_total);
    fclose(rf);

    if (want_video) ps2_vk_screenshot(shot);
    else if (shot && *shot) {
        u32 w, h, stride, x, y;
        const u8 *fb = ps2_gs_framebuffer(&w, &h, &stride);
        FILE *pf = fb ? fopen(shot, "wb") : NULL;
        if (pf) {
            fprintf(pf, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    fputc(fb[(size_t)y * stride + x * 4 + 0], pf);
                    fputc(fb[(size_t)y * stride + x * 4 + 1], pf);
                    fputc(fb[(size_t)y * stride + x * 4 + 2], pf);
                }
            fclose(pf);
            fprintf(stderr, "[REPLAY] framebuffer written to %s (%ux%u)\n",
                    shot, w, h);
        }
    }
    fprintf(stderr, "[REPLAY] done; see %s\n", want_video ? shot : "(no video)");
    if (want_video) ps2_video_shutdown();
    return 0;
}

const ps2_func_entry ps2_func_table[] = { { 0u, NULL } };
const unsigned       ps2_func_count = 0u;
const u32            ps2_entry_point = 0u;
const ps2_symbol     ps2_symbols[] = { { 0u, "" } };
const unsigned       ps2_symbol_count = 0u;

volatile int ps2_capture_request;
void ps2_capture_report(const char *why) { (void)why; }
unsigned ps2_vblank_budget(void) { return 0u; }
void ps2_finish(const char *why) {
    fprintf(stderr, "[REPLAY] finish: %s\n", why ? why : "?");
    exit(0);
}
