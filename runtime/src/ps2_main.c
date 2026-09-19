#include "ps2_runtime.h"
#include "ps2_settings.h"
#include "ps2_hle.h"
#include "ps2_statecap.h"
#include "ps2_capture.h"
#include "ps2_vk.h"
#include "ps2_gfxq.h"
#include "ps2_mod.h"
#include "ps2_hook.h"
#include "ps2_modapi.h"
#include "ps2_vfs.h"
#include "rn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern const u32 ps2_image_base;
extern const u32 ps2_image_size;

void ps2_build_dispatch(void);
void ps2_gs_init(void);
void ps2_vu1_init(void);
void ps2_sif_init(void);
void ps2_kernel_init(void);
void ps2_kernel_vblank(ps2_ctx *ctx);
u64  ps2_kernel_vblank_count(void);
void ps2_kernel_report(void);
void ps2_gs_display_report(void);
void ps2_gs_stats(u64 *prims, u64 *pixels, u64 *regs);
void ps2_gs_trx_stats(u64 *transfers, u64 *pixels);
void ps2_gs_trx_census_report(void);
void ps2_gs_tex_census_report(void);
void ps2_gs_texovl_report(void);
void ps2_gs_shadow_report(void);
void ps2_gs_fill_report(void);
void ps2_gs_draw_census_report(void);
void ps2_gs_rt_report(void);
void ps2_gs_reg_hist_report(void);
void ps2_gif_tag_report(void);
void ps2_vu_stats(u32 *unknown_lower_ops);
void ps2_vu_counters(u64 *mpg, u64 *mscal, u64 *xgkick, u64 *unpack);
void ps2_vu_lower_hist_report(void);
void ps2_vu_micro_report(void);
void ps2_gs_tex_cache_report(void);
void ps2_gif_kick_report(void);
void ps2_vu_branch_report(void);
int  ps2_vif_selftest(void);
int  ps2_gs_selftest(void);
int  ps2_spu2_selftest(void);
void ps2_spu2_init(void);
void ps2_spu2_report(void);
int  ps2_vu_selftest(void);
void ps2_vu_dump(void);
void ps2_vif_hist_report(void);
const u8 *ps2_gs_framebuffer(u32 *w, u32 *h, u32 *stride);

static const char *image_path = "ps2_image.bin";

static int load_image(const char *dir) {
    char path[1024];
    FILE *fp;
    long n;
    u8 *dst;
    snprintf(path, sizeof(path), "%s%s%s", dir ? dir : "",
             (dir && *dir) ? "/" : "", image_path);
    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cannot open memory image '%s'\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if ((u32)n != ps2_image_size)
        ps2_log("warning: image is %ld bytes, generator recorded %u",
                n, ps2_image_size);
    dst = ps2_ram + (ps2_image_base & (PS2_RAM_SIZE - 1u));
    if (fread(dst, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        fprintf(stderr, "short read on memory image\n");
        return -1;
    }
    fclose(fp);
    ps2_log("image: %ld bytes loaded at %08X", n, ps2_image_base);
    return 0;
}

static void dump_framebuffer(const char *path) {
    u32 w, h, stride;
    const u8 *fb = ps2_gs_framebuffer(&w, &h, &stride);
    FILE *fp;
    if (!fb) return;
    fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (u32 y = 0; y < h; y++) {
        const u8 *row = fb + (size_t)y * stride;
        for (u32 x = 0; x < w; x++) {
            fputc(row[x * 4 + 0], fp);
            fputc(row[x * 4 + 1], fp);
            fputc(row[x * 4 + 2], fp);
        }
    }
    fclose(fp);
    ps2_log("framebuffer written to %s (%ux%u)", path, w, h);
}

static volatile unsigned watchdog_seconds = 10;
static volatile int watchdog_armed = 1;

static void *watchdog_main(void *arg) {
    unsigned quiet = 0;
    u64 last = 0;
    (void)arg;
    if (watchdog_seconds == 0) return NULL;
    while (watchdog_armed) {
        u64 now;
        sleep(1);
        now = ps2_kernel_vblank_count();
        if (now != last) { last = now; quiet = 0; continue; }
        if (++quiet < watchdog_seconds) continue;
        if (!watchdog_armed) return NULL;
        ps2_log("");
        ps2_log("==== WATCHDOG: the guest delivered no field for %u seconds "
                "(%llu so far) ====", quiet, (unsigned long long)now);
        ps2_dump_trace("watchdog");
        ps2_kernel_report();
        exit(2);
    }
    return NULL;
}

static unsigned max_vblanks;
static int video_active;

void ps2_audio_start(void);
void ps2_audio_stop(void);
void ps2_audio_report(void);
void ps2_finish(const char *why) {
    static volatile int finishing;
    u64 prims, pixels, regs, vf, vd, vv, vt;
    if (__atomic_test_and_set(&finishing, __ATOMIC_SEQ_CST)) {
        for (;;) sleep(1);
    }
    u32 vu_unknown;
    watchdog_armed = 0;
    ps2_gfxq_drain();
    ps2_gs_stats(&prims, &pixels, &regs);
    ps2_vu_stats(&vu_unknown);
    ps2_vk_stats(&vf, &vd, &vv, &vt);
    ps2_log("---- run summary (%s) ----", why);
    ps2_log("GS register writes  : %llu", (unsigned long long)regs);
    ps2_log("GS primitives drawn : %llu", (unsigned long long)prims);
    ps2_log("GS pixels written   : %llu", (unsigned long long)pixels);
    {
        u64 ntrx, ntrxpix;
        ps2_gs_trx_stats(&ntrx, &ntrxpix);
        ps2_log("GS host->local xfers: %llu (%llu pixels)",
                (unsigned long long)ntrx, (unsigned long long)ntrxpix);
        ps2_gs_tex_cache_report();
        ps2_gs_shadow_report();
        ps2_gs_reg_hist_report();
        ps2_gif_tag_report();
        if (ps2_verbose || getenv("PS2_CENSUS")) {
            ps2_gs_trx_census_report();
            ps2_gs_tex_census_report();
            ps2_gs_texovl_report();
            ps2_gs_fill_report();
            ps2_gs_draw_census_report();
        }
        ps2_gs_rt_report();
    }
    ps2_log("VU lower opcodes unimplemented: %u", vu_unknown);
    {
        u64 mpg, mscal, xgk, unp;
        ps2_vu_counters(&mpg, &mscal, &xgk, &unp);
        ps2_log("VU microprogram uploads/starts/xgkicks: %llu / %llu / %llu",
                (unsigned long long)mpg, (unsigned long long)mscal,
                (unsigned long long)xgk);
        ps2_vif_hist_report();
        ps2_vu_micro_report();
        ps2_gif_kick_report();
        ps2_vu_branch_report();
    }
    ps2_gfxq_report();
    ps2_sif_hle_report();
    ps2_cdvd_report();
    ps2_mod_report();
    ps2_hook_report();
    rn_tap_report();
    rn_intent_report();
    rn_fixes_report();
    rn_report();
    ps2_lua_report();
    ps2_nufile_report();
    ps2_nusound_report();
    ps2_nusndstr_report();
    ps2_spu2_report();
    ps2_audio_report();
    ps2_pad_report();
    ps2_pad_io_report();
    ps2_ipu_report();
    ps2_video_report();
    ps2_unknown_report();
    ps2_kernel_report();
    ps2_gs_display_report();
    {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            u64 kt = ((u64)k.dwHighDateTime << 32) | k.dwLowDateTime;
            u64 ut = ((u64)u.dwHighDateTime << 32) | u.dwLowDateTime;
            double cpu = (double)(kt + ut) / 1e7;
            double wall = ps2_wall_seconds();
            ps2_log("cpu: %.1f s of CPU over %.1f s wall = %.2f cores busy "
                    "(the guest spin-waits, so this is not headroom)",
                    cpu, wall, wall > 0.0 ? cpu / wall : 0.0);
        }
#endif
    }
    ps2_phase_report(ps2_kernel_vblank_count());
    ps2_sampler_report(24);
    ps2_host_prof_report();
    ps2_prof_report(30);
    ps2_dump_trace("run summary");
    if (video_active) {
        ps2_video_shutdown();
        ps2_vk_screenshot("ps2_frame.ppm");
    } else {
        dump_framebuffer("ps2_framebuffer.ppm");
    }
    ps2_cap_report();
    ps2_state_dump_if_armed();
    fflush(stdout);
    _exit(0);
}

unsigned ps2_vblank_budget(void) { return max_vblanks; }

volatile int ps2_capture_request;
static u64 capture_n;

void ps2_capture_report(const char *why) {
    u64 prims, pixels, regs, mpg, mscal, xgk, unp;
    u32 vu_unknown;
    capture_n++;
    ps2_log("");
    ps2_log("======== CAPTURE %llu (%s) ========",
            (unsigned long long)capture_n, why ? why : "?");
    ps2_gs_stats(&prims, &pixels, &regs);
    ps2_log("GS register writes  : %llu", (unsigned long long)regs);
    ps2_log("GS primitives drawn : %llu", (unsigned long long)prims);
    {
        u64 ntrx, ntrxpix;
        ps2_gs_trx_stats(&ntrx, &ntrxpix);
        ps2_log("GS host->local xfers: %llu (%llu pixels)",
                (unsigned long long)ntrx, (unsigned long long)ntrxpix);
    }
    ps2_gs_reg_hist_report();
    ps2_gif_tag_report();
    ps2_gs_trx_census_report();
    ps2_gs_tex_census_report();
    ps2_gs_texovl_report();
    ps2_gs_fill_report();
    ps2_gs_draw_census_report();
    ps2_gs_rt_report();
    ps2_vu_stats(&vu_unknown);
    ps2_vu_counters(&mpg, &mscal, &xgk, &unp);
    ps2_log("VU microprogram uploads/starts/xgkicks/unpacks: "
            "%llu / %llu / %llu / %llu",
            (unsigned long long)mpg, (unsigned long long)mscal,
            (unsigned long long)xgk, (unsigned long long)unp);
    ps2_log("VU lower opcodes unimplemented: %u", vu_unknown);
    ps2_vu_micro_report();
    ps2_vu_dump();
    ps2_vu_lower_hist_report();
    ps2_vif_hist_report();
    ps2_video_report();
    rn_census_report("F9");
    ps2_unknown_report();
    ps2_dump_trace("capture");
    {
        char nm[64];
        snprintf(nm, sizeof nm, "capture_%llu.ppm",
                 (unsigned long long)capture_n);
        if (video_active) ps2_vk_screenshot(nm);
        else dump_framebuffer(nm);
    }
    ps2_log("======== END CAPTURE %llu ========",
            (unsigned long long)capture_n);
    ps2_log("");
    printf("[F9] capture %llu written (capture_%llu.ppm); "
           "keep playing or press Esc\n",
           (unsigned long long)capture_n, (unsigned long long)capture_n);
    fflush(stdout);
    fflush(stderr);
}

static int nufile_selftest(const char *disc_file) {
    enum { REQ = 0x00300000u, BUF = 0x00310000u, REF = 4096 };
    const ps2_disc_file *f = ps2_vfs_find(disc_file);
    u8 *ref;
    u32 i, n, fd;
    int rc, fails = 0;

    if (!f) {
        ps2_log("selftest: '%s' is not on the disc", disc_file);
        return 1;
    }
    n = f->size < REF ? f->size : REF;
    ref = (u8 *)malloc(n + 4096);
    if (!ref) return 1;
    if (ps2_vfs_read_sectors(f->lsn, (n + 256 + 2047) / 2048, ref) <= 0) {
        ps2_log("selftest: cannot read '%s' off the disc", disc_file);
        free(ref);
        return 1;
    }

    for (i = 0; i < 64; i++) ps2_w32(REQ + i * 4, 0);
    ps2_w32(REQ + 0x18, (u32)strlen(disc_file) + 1);
    for (i = 0; i <= strlen(disc_file); i++)
        ps2_w8(REQ + 0x1C + i, (u8)disc_file[i]);
    rc = ps2_rpc_call(0x4E554649u, 1, REQ, 1024, REQ, 16);
    fd = ps2_r32(REQ);
    ps2_log("selftest: nufile open('%s') -> rc %d, handle %d", disc_file, rc,
            (int)fd);
    if ((int)fd <= 0) fails++;

    for (i = 0; i < n; i++) ps2_w8(BUF + i, 0xA5);
    ps2_w32(REQ + 0x08, fd);
    ps2_w32(REQ + 0x0C, BUF);
    ps2_w32(REQ + 0x10, n);
    rc = ps2_rpc_call(0x4E554649u, 3, REQ, 32, REQ, 16);
    {
        u32 got = ps2_r32(REQ), bad = 0;
        for (i = 0; i < n; i++) if (ps2_r8(BUF + i) != ref[i]) bad++;
        ps2_log("selftest: nufile read(%u) -> %u bytes, %u of %u differ",
                n, got, bad, n);
        if (got != n || bad) fails++;
    }

    ps2_w32(REQ + 0x08, fd);
    ps2_w32(REQ + 0x0C, BUF);
    ps2_w32(REQ + 0x10, 256);
    rc = ps2_rpc_call(0x4E554649u, 4, REQ, 32, REQ, 16);
    {
        u32 got = ps2_r32(REQ), bad = 0;
        u32 want = f->size - n < 256u ? f->size - n : 256u;
        for (i = 0; i < want && n + i < REF + 2048; i++)
            if (ps2_r8(BUF + i) != ref[n + i]) bad++;
        ps2_log("selftest: nufile read(256) at offset %u -> %u bytes "
                "(%u available), %u differ", n, got, want, bad);
        if (got != want || bad) fails++;
    }

    ps2_w32(REQ + 0x08, fd);
    ps2_rpc_call(0x4E554649u, 2, REQ, 32, REQ, 16);

    for (i = 0; i < 64; i++) ps2_w32(REQ + i * 4, 0);
    for (i = 0; i < n; i++) ps2_w8(BUF + i, 0x5A);
    ps2_w32(REQ + 0x08, n);
    ps2_w32(REQ + 0x0C, BUF);
    ps2_w32(REQ + 0x10, (u32)strlen(disc_file) + 1);
    for (i = 0; i <= strlen(disc_file); i++)
        ps2_w8(REQ + 0x14 + i, (u8)disc_file[i]);
    rc = ps2_rpc_call(0x4E554649u, 8, REQ, 1024, REQ, 16);
    {
        u32 got = ps2_r32(REQ), bad = 0;
        for (i = 0; i < n; i++) if (ps2_r8(BUF + i) != ref[i]) bad++;
        ps2_log("selftest: nufile load('%s', %u) -> %u bytes, %u differ",
                disc_file, n, got, bad);
        if (got != n || bad) fails++;
    }
    free(ref);
    ps2_log("selftest: %s", fails ? "FAILED" : "all NUFILE checks passed");
    return fails;
}

#ifdef _WIN32
static LONG WINAPI ps2_crash_filter(EXCEPTION_POINTERS *ep) {
    unsigned long code = ep && ep->ExceptionRecord
                       ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0;
    void *at = ep && ep->ExceptionRecord
             ? ep->ExceptionRecord->ExceptionAddress : NULL;
    {
        HMODULE base = GetModuleHandleA(NULL);
        ps2_log("crash: exception %08lX at %p  (image %p, rva %08llX)",
                code, at, (void *)base,
                (unsigned long long)((char *)at - (char *)base));
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        ps2_log("crash: %s address %p",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    {
        void *fr[32];
        USHORT n = RtlCaptureStackBackTrace(0, 32, fr, NULL);
        HMODULE self = GetModuleHandleA(NULL);
        USHORT i;
        for (i = 0; i < n; i++) {
            HMODULE m = NULL;
            char nm[MAX_PATH];
            const char *base = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)fr[i], &m)
                && GetModuleFileNameA(m, nm, sizeof nm)) {
                const char *sl = strrchr(nm, '\\');
                base = sl ? sl + 1 : nm;
            }
            ps2_log("crash:   frame %2u  %p  %s+%08llX%s", i, fr[i], base,
                    (unsigned long long)((char *)fr[i] - (char *)m),
                    m == self ? "   <-- ours" : "");
        }
    }
    ps2_dump_trace("crash");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char **argv) {
    ps2_settings_defaults(&ps2_cfg);
    const char *dir = ".";
    const char *disc = NULL;
    int want_video = 1, want_profile = 0, want_selftest = 0, want_vif_test = 0, want_vu_test = 0;
    int want_gs_test = 0, want_spu2_test = 0, want_hle_test = 0;
    int want_autoplay = 0, want_deint = 1;
    int want_stop_scene[3] = { -1, -1, 90 }, have_stop_scene = 0;
    int want_hidden = 0;
    const char *cap_path = NULL;
    int cap_frames = -1, cap_level = -1;
    long cap_at = -1;
    const char *ipu_test_file = NULL;
    int ipu_test_frames = 0;
    unsigned frames = 0, max_frames = 0;
    double budget = 10.0;
    clock_t t0;
#ifdef _WIN32
    SetUnhandledExceptionFilter(ps2_crash_filter);
#endif

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-state") && i + 1 < argc)
            ps2_state_dump_arm(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            budget = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc)
            watchdog_seconds = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--novideo")) want_video = 0;
        else if (!strcmp(argv[i], "--hidden")) want_hidden = 1;
        else if (!strcmp(argv[i], "--selftest")) want_selftest = 1;
        else if (!strcmp(argv[i], "--gs-test")) want_gs_test = 1;
        else if (!strcmp(argv[i], "--spu2-test")) want_spu2_test = 1;
        else if (!strcmp(argv[i], "--hle-test")) want_hle_test = 1;
        else if (!strcmp(argv[i], "--vif-test")) want_vif_test = 1;
        else if (!strcmp(argv[i], "--vu-test")) want_vu_test = 1;
        else if (!strcmp(argv[i], "--ipu-test") && i + 1 < argc)
            ipu_test_file = argv[++i];
        else if (!strcmp(argv[i], "--ipu-frames") && i + 1 < argc)
            ipu_test_frames = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
            cap_path = argv[++i];
        else if (!strcmp(argv[i], "--capture-frames") && i + 1 < argc)
            cap_frames = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--capture-seconds") && i + 1 < argc)
            cap_frames = (int)(strtod(argv[++i], NULL) * 59.94);
        else if (!strcmp(argv[i], "--capture-level") && i + 1 < argc) {
            const char *a = argv[++i];
            cap_level = !strcmp(a, "gs") ? PS2_CAP_LEVEL_GS
                                         : PS2_CAP_LEVEL_DMA;
        }
        else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc)
            cap_at = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--autoplay")) want_autoplay = 1;
        else if (!strcmp(argv[i], "--no-deinterlace")) want_deint = 0;
        else if (!strcmp(argv[i], "--verbose")) ps2_verbose = 1;
        else if (!strcmp(argv[i], "--profile")) want_profile = 1;
        else if (!strcmp(argv[i], "--trace-depth") && i + 1 < argc)
            ps2_trace_show = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vblanks") && i + 1 < argc)
            max_vblanks = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--stop-scene") && i + 1 < argc) {
            const char *a = argv[++i];
            char *e;
            long mj = strtol(a, &e, 10), mn = -1, fr = 90;
            if (*e == '.') mn = strtol(e + 1, &e, 10);
            if (*e == ':') fr = strtol(e + 1, &e, 10);
            want_stop_scene[0] = (int)mj;
            want_stop_scene[1] = (int)mn;
            want_stop_scene[2] = (int)fr;
            have_stop_scene = 1;
        }
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--data DIR] [--disc ISO-or-DIR]\n"
                   "          [--vblanks N] [--watchdog N] [--seconds S]\n"
                   "          [--novideo] [--hidden] [--verbose] [--profile]\n"
                   "          [--autoplay] [--selftest] [--gs-test] [--hle-test] [--spu2-test]\n"
                   "          [--ipu-test FILE.ipu [--ipu-frames N]]\n"
                   "          [--no-deinterlace] [--stop-scene M.N[:frames]]\n"
                   "          [--capture FILE] [--capture-seconds S |"
                   " --capture-frames N]\n"
                   "          [--capture-level dma|gs] [--capture-at FIELD]\n"
                   "  --capture     where F6 writes; default out/capture.gscap\n"
                   "  --capture-*   length, level and an automatic start.\n"
                   "                level dma (default) replays the whole\n"
                   "                graphics machine, gs only the renderer.\n"
                   "                Replay it with gsreplay --loop 0 --bench.\n"
                   "  --vblanks 0   run until the window is closed (default)\n"
                   "  --stop-scene  finish once the game has been in scene\n"
                   "                state M.N for that many frames (90)\n"
                   "  --watchdog N  give up if no field is delivered for N seconds\n"
                   "                (0 never gives up)\n",
                   argv[0]);
            return 0;
        }
    }

    {   const char *w = getenv("PS2_WATCH_MEM");
#if !PS2_DIAG
        if (w)
            ps2_log("PS2_WATCH_MEM: the guest-store watch is not in this "
                    "build.  It is a test on every guest store and costs "
                    "16-18%% of the generated code, so it is a build option "
                    "now: cmake -B build -DPS2_DIAG=ON");
        w = NULL;
#endif
        if (w) {
            const char *colon = strchr(w, ':');
            ps2_watch_addr = (u32)strtoul(w, NULL, 0) & ~3u;
            ps2_watch_last = colon ? ((u32)strtoul(colon + 1, NULL, 0) & ~3u)
                                   : ps2_watch_addr;
            if (ps2_watch_last < ps2_watch_addr)
                ps2_watch_last = ps2_watch_addr;
            if (ps2_watch_last == ps2_watch_addr)
                ps2_log("watch: reporting every store to %08X",
                        ps2_watch_addr);
            else
                ps2_log("watch: reporting every store to %08X..%08X",
                        ps2_watch_addr, ps2_watch_last);
        } }
    ps2_log("ps2recomp runtime: %u recompiled functions, entry %08X",
            ps2_func_count, ps2_entry_point);
    if (want_profile) {
#if PS2_DIAG
        ps2_prof_enable(0x00100000u, 0x00300000u);
#else
        ps2_log("--profile: the entry histogram is not in this build.  It is a "
                "test on every recompiled function call and costs 5-11%% of the "
                "generated code, so it is a build option now: "
                "cmake -B build -DPS2_DIAG=ON");
#endif
    }
    if (ipu_test_file) {
        ps2_ipu_init();
        return ps2_ipu_selftest(ipu_test_file, ipu_test_frames);
    }
    {
        const char *census = getenv("PS2_CENSUS");
        ps2_diag_armed = (census && atoi(census) != 0) || ps2_verbose ||
            getenv("PS2_DUMP_TEX") || getenv("PS2_TEX_DETAIL") ||
            getenv("PS2_TRACE_VU") || getenv("PS2_VU_WATCH") ||
            getenv("PS2_VU_PROFILE");
        ps2_log("diag: per-primitive censuses %s (PS2_CENSUS=1 enables; captures can arm temporarily)",
                ps2_diag_armed ? "enabled" : "disabled");
    }
    ps2_phase_init();
    ps2_sampler_start();
    ps2_host_prof_attach("ee");
    ps2_wall_seconds();
    ps2_mem_init();
    ps2_statecap_init(ps2_ram, PS2_RAM_SIZE);
    ps2_gs_init();
    ps2_vu1_init();
    if (want_gs_test) return ps2_gs_selftest();
    if (want_hle_test) {
        extern int ps2_hle_selftest(const char *disc);
        return ps2_hle_selftest(disc);
    }
    if (want_spu2_test) {
        extern int ps2_nustream_file_selftest(const char *disc);
        extern int ps2_nusound_packet_selftest(void);
        extern int ps2_nusndstr_address_selftest(void);
        extern int ps2_nusndstr_fx_send_selftest(void);
        int rc = ps2_spu2_selftest();
        rc |= ps2_nusndstr_address_selftest();
        rc |= ps2_nusndstr_fx_send_selftest();
        rc |= ps2_nusound_packet_selftest();
        if (disc) rc |= ps2_nustream_file_selftest(disc);
        return rc;
    }
    if (want_vif_test) return ps2_vif_selftest();
    if (want_vu_test) return ps2_vu_selftest();
    ps2_ipu_init();
    ps2_sif_init();
    ps2_sif_hle_init();
    ps2_cdvd_init();
    ps2_cdvd_rpc_register();
    ps2_iop_services_register();
    ps2_pad_init();
    ps2_kernel_init();
    ps2_pad_autoplay(want_autoplay);
    if (have_stop_scene)
        ps2_stop_scene(want_stop_scene[0], want_stop_scene[1],
                       want_stop_scene[2]);
    ps2_gs_deinterlace(!want_deint);
    if (cap_path || cap_frames >= 0 || cap_level >= 0 || cap_at >= 0) {
        ps2_mkdir_p("out");
        ps2_cap_configure(cap_path, cap_frames, cap_level);
        if (cap_at >= 0) ps2_cap_arm_at((u32)cap_at);
        ps2_gfxq_forbid("a capture is configured");
    }
    ps2_gfxq_init();
    ps2_build_dispatch();
    if (getenv("PS2_HOOK_TEST")) {
        extern void ps2_hook_selftest_attach(void);
        ps2_hook_selftest_attach();
    }
    if (load_image(dir) != 0) return 1;
    ps2_mod_init();
    if (disc && ps2_vfs_open(disc) != 0)
        ps2_log("warning: no disc at '%s'; CDVD requests will fail", disc);
    ps2_mod_start();
    rn_init();
    rn_dump_init();
    rn_vp_init();
    rn_intent_init();
    rn_fixes_init();
    if (want_selftest) {
        int bad;
        ps2_sif_hle_init();
        ps2_cdvd_rpc_register();
        ps2_iop_services_register();
        bad  = nufile_selftest("\\BIN\\DATA.TBL;1");
        bad += nufile_selftest("\\SYSTEM.CNF;1");
        return bad ? 1 : 0;
    }
    ps2_settings_load();
    ps2_video_hidden(want_hidden);
    if (want_video && ps2_video_init("Ace Combat 5 - The Unsung War", 640, 448) != 0) {
        ps2_log("warning: no video output; running headless");
        want_video = 0;
    }
    video_active = want_video;
    ps2_audio_start();

    ps2_cpu.r[29].ud[0] = 0x01FFFF00u;
    ps2_cpu.r[28].ud[0] = 0;
    ps2_cpu.r[4].ud[0] = 0;
    ps2_cpu.r[5].ud[0] = 0;
    ps2_cpu.r[31].ud[0] = 0;
    ps2_cpu.cop0[12] = 0x70000000u;

    {
        pthread_t wd;
        pthread_create(&wd, NULL, watchdog_main, NULL);
        pthread_detach(wd);
    }

    t0 = clock();
    ps2_log("entering recompiled guest at %08X", ps2_entry_point);
    ps2_dispatch(&ps2_cpu, ps2_entry_point);
    watchdog_armed = 0;
    ps2_log("guest entry returned after %.2fs",
            (double)(clock() - t0) / CLOCKS_PER_SEC);

    while ((max_frames == 0 || frames < max_frames)
           && (double)(clock() - t0) / CLOCKS_PER_SEC < budget) {
        ps2_kernel_vblank(&ps2_cpu);
        frames++;
        if (max_frames == 0 && frames > 100000) break;
    }

    {
        u64 prims, pixels, regs;
        u32 vu_unknown;
        ps2_gfxq_drain();
        ps2_gs_stats(&prims, &pixels, &regs);
        ps2_vu_stats(&vu_unknown);
        ps2_log("---- run summary ----");
        ps2_log("vblanks pumped      : %u", frames);
        ps2_log("GS register writes  : %llu", (unsigned long long)regs);
        ps2_log("GS primitives drawn : %llu", (unsigned long long)prims);
        ps2_log("GS pixels written   : %llu", (unsigned long long)pixels);
    {
        u64 ntrx, ntrxpix;
        ps2_gs_trx_stats(&ntrx, &ntrxpix);
        ps2_log("GS host->local xfers: %llu (%llu pixels)",
                (unsigned long long)ntrx, (unsigned long long)ntrxpix);
        ps2_gs_tex_cache_report();
        ps2_gs_reg_hist_report();
        ps2_gif_tag_report();
    }
        ps2_log("VU lower opcodes unimplemented: %u", vu_unknown);
    {
        u64 mpg, mscal, xgk, unp;
        ps2_vu_counters(&mpg, &mscal, &xgk, &unp);
        ps2_log("VU microprogram uploads/starts/xgkicks: %llu / %llu / %llu",
                (unsigned long long)mpg, (unsigned long long)mscal,
                (unsigned long long)xgk);
        ps2_vif_hist_report();
    }
        ps2_gfxq_report();
        ps2_sif_hle_report();
        ps2_cdvd_report();
    ps2_nufile_report();
    ps2_nusound_report();
    ps2_nusndstr_report();
    ps2_spu2_report();
        ps2_pad_report();
        ps2_video_report();
        ps2_kernel_report();
        ps2_dump_trace("end of run");
    }
    dump_framebuffer("ps2_framebuffer.ppm");
    ps2_gfxq_shutdown();
    ps2_audio_stop();
    if (want_video) ps2_video_shutdown();
    return 0;
}
