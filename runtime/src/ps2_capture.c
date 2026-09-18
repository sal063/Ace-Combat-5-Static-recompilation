#include "ps2_capture.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#endif

void ps2_gs_stats(u64 *prims, u64 *pixels, u64 *regs);
void ps2_gs_trx_stats(u64 *transfers, u64 *pixels);
extern u32 ps2_gs_frame_count;

int g_cap_recording = 0;
int g_cap_deep      = 0;
int g_cap_shallow   = 0;
int g_cap_disable   = 0;

void ps2_mkdir_p(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

#define CAP_DICT_SLOTS_DEFAULT  8192u
#define CAP_DICT_MIN_DEFAULT    2048u
#define CAP_DICT_MAX_DEFAULT   (2u << 20)
#define CAP_DICT_BUDGET_DEFAULT (256ull << 20)

typedef struct { u8 *raw, *p; u32 len; } cap_dict_ent;

static u8 *cap_align16(u8 *p) {
    return (u8 *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
}

static cap_dict_ent *dict;
static u32 dict_slots = CAP_DICT_SLOTS_DEFAULT;
static u32 dict_min   = CAP_DICT_MIN_DEFAULT;
static u32 dict_max   = CAP_DICT_MAX_DEFAULT;
static u64 dict_budget = CAP_DICT_BUDGET_DEFAULT;
static u64 dict_held, dict_hits, dict_inserts, dict_saved;

static u32 cap_pow2(u32 v) {
    u32 p = 1u;
    while (p < v && p < (1u << 24)) p <<= 1;
    return p;
}

void ps2_cap_dict_config(u32 slots, u32 lo, u32 hi, u64 budget) {
    ps2_cap_dict_reset();
    free(dict);
    dict = NULL;
    dict_slots  = cap_pow2(slots ? slots : CAP_DICT_SLOTS_DEFAULT);
    dict_min    = lo ? lo : CAP_DICT_MIN_DEFAULT;
    dict_max    = hi ? hi : CAP_DICT_MAX_DEFAULT;
    dict_budget = budget ? budget : CAP_DICT_BUDGET_DEFAULT;
    if (dict_max < dict_min) dict_max = dict_min;
}

void ps2_cap_dict_reset(void) {
    u32 i;
    if (dict)
        for (i = 0; i < dict_slots; i++) {
            free(dict[i].raw);
            dict[i].raw = NULL;
            dict[i].p = NULL;
            dict[i].len = 0;
        }
    dict_held = dict_hits = dict_inserts = dict_saved = 0;
}

static int dict_ready(void) {
    if (dict) return 1;
    dict = (cap_dict_ent *)calloc(dict_slots, sizeof *dict);
    return dict != NULL;
}

int ps2_cap_dict_eligible(u32 len) {
    return len >= dict_min && len <= dict_max;
}

static u32 dict_hash(const u8 *p, u32 len) {
    u64 h = 1469598103934665603ull ^ (u64)len;
    u32 i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return (u32)((h ^ (h >> 32)) & (dict_slots - 1u));
}

int ps2_cap_dict_lookup(const void *p, u32 len, u32 *slot) {
    u32 s;
    if (!ps2_cap_dict_eligible(len) || !dict_ready()) return 0;
    s = dict_hash((const u8 *)p, len);
    if (slot) *slot = s;
    if (dict[s].p && dict[s].len == len && !memcmp(dict[s].p, p, len)) {
        dict_hits++;
        dict_saved += len;
        return 1;
    }
    return 0;
}

void ps2_cap_dict_insert(const void *p, u32 len) {
    u32 s;
    u64 after;
    if (!ps2_cap_dict_eligible(len) || !dict_ready()) return;
    s = dict_hash((const u8 *)p, len);
    after = dict_held - dict[s].len + len;
    if (after > dict_budget) return;
    {
        u8 *n = (u8 *)malloc((size_t)len + 16u);
        if (!n) return;
        memcpy(cap_align16(n), p, len);
        free(dict[s].raw);
        dict_held = after;
        dict[s].raw = n;
        dict[s].p = cap_align16(n);
        dict[s].len = len;
        dict_inserts++;
    }
}

const u8 *ps2_cap_dict_get(u32 slot, u32 *len) {
    if (!dict || slot >= dict_slots) return NULL;
    if (len) *len = dict[slot].len;
    return dict[slot].p;
}

void ps2_cap_dict_stats(u64 *hits, u64 *inserts, u64 *held, u64 *saved) {
    if (hits) *hits = dict_hits;
    if (inserts) *inserts = dict_inserts;
    if (held) *held = dict_held;
    if (saved) *saved = dict_saved;
}

#define CAP_BUFSZ (8u << 20)
#define CAP_BUFS  3

static FILE *g_cf;
static char  g_path[512];
static char  g_cfg_path[512] = "out/capture.gscap";
static int   g_cfg_frames = 3600;
static int   g_cfg_level  = PS2_CAP_LEVEL_DMA;
static char  g_reqpath[512];
static int   g_reqframes, g_reqlevel;
static volatile int g_req_pending;
static volatile int g_stop_req;

static u64   g_nrec;
static u64   g_frame_nrec;
static u64   g_stream_pos;
static u64   g_stream_off;
static u64   g_bytes_written;
static u64   g_max_bytes = 16ull << 30;
static int   g_frames_left;
static int   g_frames_done;
static int   g_empty;
static int   g_truncated;
static u32   g_first_field;
static double g_wall0;
static ps2_cap_hdr g_hdr;

static u64   g_last_regs, g_last_prims, g_last_trx;

static ps2_cap_ixent *g_index;
static u32   g_index_n, g_index_cap;

static u8   *g_buf;
static size_t g_buf_n;
static u8   *g_free[CAP_BUFS];
static int   g_nfree;
static struct { u8 *p; size_t n; } g_full[CAP_BUFS];
static int   g_nfull;
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_cv_free = PTHREAD_COND_INITIALIZER;
static pthread_t g_writer;
static int   g_writer_live;
static int   g_writer_stop;
static u64   g_stall_blocks;

#define CAP_NORUN ((size_t)-1)
static size_t g_run_pos = CAP_NORUN;
static u32    g_run_n;

static void *cap_writer_main(void *arg) {
    (void)arg;
    for (;;) {
        u8 *p = NULL;
        size_t n = 0;
        pthread_mutex_lock(&g_m);
        while (!g_nfull && !g_writer_stop)
            pthread_cond_wait(&g_cv_full, &g_m);
        if (g_nfull) {
            p = g_full[0].p;
            n = g_full[0].n;
            memmove(g_full, g_full + 1, sizeof(g_full[0]) * (size_t)(--g_nfull));
        } else {
            pthread_mutex_unlock(&g_m);
            break;
        }
        pthread_mutex_unlock(&g_m);
        if (g_cf && n) fwrite(p, 1, n, g_cf);
        pthread_mutex_lock(&g_m);
        g_free[g_nfree++] = p;
        pthread_cond_signal(&g_cv_free);
        pthread_mutex_unlock(&g_m);
    }
    return NULL;
}

static void cap_flush_buffer(void) {
    if (!g_buf) return;
    if (!g_writer_live) {
        if (g_cf && g_buf_n) fwrite(g_buf, 1, g_buf_n, g_cf);
        g_buf_n = 0;
        g_run_pos = CAP_NORUN;
        return;
    }
    pthread_mutex_lock(&g_m);
    if (g_buf_n) {
        g_full[g_nfull].p = g_buf;
        g_full[g_nfull].n = g_buf_n;
        g_nfull++;
        pthread_cond_signal(&g_cv_full);
        g_buf = NULL;
        while (!g_nfree) {
            g_stall_blocks++;
            pthread_cond_wait(&g_cv_free, &g_m);
        }
        g_buf = g_free[--g_nfree];
    }
    pthread_mutex_unlock(&g_m);
    g_buf_n = 0;
    g_run_pos = CAP_NORUN;
}

static void cap_writer_join(void) {
    if (!g_writer_live) return;
    cap_flush_buffer();
    pthread_mutex_lock(&g_m);
    g_writer_stop = 1;
    pthread_cond_broadcast(&g_cv_full);
    pthread_mutex_unlock(&g_m);
    pthread_join(g_writer, NULL);
    g_writer_live = 0;
    g_writer_stop = 0;
}

static int g_stopping;

PS2_INLINE int cap_room(size_t n) {
    if (!g_buf) return 0;
    if (g_buf_n + n <= CAP_BUFSZ) return 1;
    cap_flush_buffer();
    return g_buf && g_buf_n + n <= CAP_BUFSZ;
}

PS2_INLINE void cap_raw(const void *p, size_t n) {
    memcpy(g_buf + g_buf_n, p, n);
    g_buf_n += n;
    g_stream_pos += n;
    g_bytes_written += n;
}

PS2_INLINE void cap_u8(u8 v)  { cap_raw(&v, 1); }
PS2_INLINE void cap_u32(u32 v){ cap_raw(&v, 4); }
PS2_INLINE void cap_u64(u64 v){ cap_raw(&v, 8); }

static void cap_bytes(const u8 *p, u32 len) {
    while (len) {
        u32 take;
        if (g_buf_n == CAP_BUFSZ) cap_flush_buffer();
        if (!g_buf) return;
        take = (u32)(CAP_BUFSZ - g_buf_n);
        if (take > len) take = len;
        cap_raw(p, take);
        p += take;
        len -= take;
    }
}

PS2_INLINE int cap_begin(u8 op, size_t payload) {
    if (!g_cap_recording) return 0;
    if (g_bytes_written >= g_max_bytes && !g_stopping) {
        g_truncated = 1;
        ps2_cap_stop("size limit reached");
        return 0;
    }
    if (!cap_room(1 + payload)) return 0;
    g_run_pos = CAP_NORUN;
    cap_u8(op);
    g_nrec++;
    return 1;
}

void ps2_cap_reg(u32 reg, u64 val) {
    if (!g_cap_shallow) return;
    if (reg == 0x54u) {
        if (g_run_pos != CAP_NORUN && g_run_n < 0x00FFFFFFu
            && g_buf && g_buf_n + 8u <= CAP_BUFSZ) {
            cap_u64(val);
            g_run_n++;
            memcpy(g_buf + g_run_pos, &g_run_n, 4);
            return;
        }
        if (!cap_begin(PS2_CAP_OP_HWRUN, 4 + 8)) return;
        g_run_pos = g_buf_n;
        g_run_n = 1;
        cap_u32(1u);
        cap_u64(val);
        return;
    }
    if (!cap_begin(PS2_CAP_OP_REG, 1 + 8)) return;
    cap_u8((u8)reg);
    cap_u64(val);
}

void ps2_cap_priv(u32 addr, u64 val) {
    if (!g_cap_recording) return;
    if (!cap_begin(PS2_CAP_OP_PRIV, 4 + 8)) return;
    cap_u32(addr);
    cap_u64(val);
}

static void cap_payload(u8 op_inline, u8 op_ref, int ch, const void *p,
                        u32 qwc) {
    u32 len = qwc * 16u;
    u32 slot = 0;
    if (!qwc) return;
    if (ps2_cap_dict_lookup(p, len, &slot)) {
        if (!cap_begin(op_ref, (ch >= 0 ? 1u : 0u) + 4)) return;
        if (ch >= 0) cap_u8((u8)ch);
        cap_u32(slot);
        return;
    }
    if (!cap_begin(op_inline, (ch >= 0 ? 1u : 0u) + 4)) return;
    if (ch >= 0) cap_u8((u8)ch);
    cap_u32(qwc);
    cap_bytes((const u8 *)p, len);
    ps2_cap_dict_insert(p, len);
}

void ps2_cap_vif_qw(int ch, const void *qw, u32 qwc) {
    if (!g_cap_deep) return;
    cap_payload(PS2_CAP_OP_VIFQW, PS2_CAP_OP_VIFREF, ch & 1, qw, qwc);
}

void ps2_cap_gif_qw(const void *qw, u32 qwc) {
    if (!g_cap_deep) return;
    cap_payload(PS2_CAP_OP_GIFQW, PS2_CAP_OP_GIFREF, -1, qw, qwc);
}

void ps2_cap_vif_word(int ch, u32 word) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_VIFW, 1 + 4)) return;
    cap_u8((u8)ch);
    cap_u32(word);
}

void ps2_cap_vif_reset(int ch, u32 val) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_FBRST, 1 + 4)) return;
    cap_u8((u8)(ch & 1));
    cap_u32(val);
}

void ps2_cap_vif_clear(int ch) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_VIFCLR, 1)) return;
    cap_u8((u8)(ch & 1));
}

void ps2_cap_mmio(u32 addr, u32 val) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_MMIO, 4 + 4)) return;
    cap_u32(addr);
    cap_u32(val);
}

void ps2_cap_intent(const u8 *rec, u32 len) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_INTENT, 4)) return;
    cap_u32(len);
    cap_bytes(rec, len);
}

void ps2_cap_tag(u32 kind, u32 a, u32 b) {
    if (!g_cap_deep) return;
    if (!cap_begin(PS2_CAP_OP_TAG, 1 + 4 + 4)) return;
    cap_u8((u8)kind);
    cap_u32(a);
    cap_u32(b);
}

static void cap_index_push(u32 field) {
    if (g_index_n == g_index_cap) {
        u32 want = g_index_cap ? g_index_cap * 2u : 4096u;
        ps2_cap_ixent *n = (ps2_cap_ixent *)realloc(g_index,
                                                    (size_t)want * sizeof *n);
        if (!n) return;
        g_index = n;
        g_index_cap = want;
    }
    g_index[g_index_n].off = g_stream_pos;
    g_index[g_index_n].field = field;
    g_index[g_index_n].pad = 0;
    g_index_n++;
}

int ps2_cap_active(void) { return g_cap_recording; }

void ps2_cap_configure(const char *path, int frames, int level) {
    if (path && *path) snprintf(g_cfg_path, sizeof g_cfg_path, "%s", path);
    if (frames >= 0) g_cfg_frames = frames;
    if (level >= 0) g_cfg_level = level;
}

static u32 g_arm_field;
static int g_armed_fired;

void ps2_cap_arm_at(u32 field) {
    g_arm_field = field;
    g_armed_fired = 0;
    ps2_log("cap: armed to start at field %u -- %s, %d frames, level %s",
            field, g_cfg_path, g_cfg_frames,
            g_cfg_level == PS2_CAP_LEVEL_DMA ? "dma" : "gs");
}

void ps2_cap_stop(const char *why) {
    u64 hits, ins, held, saved;
    if (!g_cap_recording || g_stopping) return;
    g_stopping = 1;
    (void)cap_begin(PS2_CAP_OP_END, 0);
    g_cap_recording = g_cap_deep = g_cap_shallow = 0;
    cap_writer_join();

    g_hdr.stream_bytes = g_stream_pos;
    g_hdr.nrec = g_nrec;
    g_hdr.nfields = g_index_n;
    g_hdr.truncated = (u32)g_truncated;
    {
        double dt = ps2_wall_seconds() - g_wall0;
        g_hdr.live_us_per_field = (g_index_n && dt > 0.0)
            ? (u32)(dt * 1e6 / (double)g_index_n) : 0u;
        if (g_hdr.live_us_per_field)
            ps2_log("cap: the game delivered these %u fields at %.1f fields/s "
                    "(%.2f ms/field)", g_index_n, (double)g_index_n / dt,
                    dt * 1000.0 / (double)g_index_n);
    }
    if (g_cf) {
        g_hdr.index_off = g_stream_off + g_stream_pos;
        if (g_index_n)
            fwrite(g_index, sizeof *g_index, g_index_n, g_cf);
        else
            g_hdr.index_off = 0;
        fseek(g_cf, 0, SEEK_SET);
        fwrite(&g_hdr, sizeof g_hdr, 1, g_cf);
        fclose(g_cf);
        g_cf = NULL;
    }
    ps2_cap_dict_stats(&hits, &ins, &held, &saved);
    ps2_log("cap: %s -- %u fields, %llu records, %.1f MB of stream%s",
            why && *why ? why : "capture closed", g_index_n,
            (unsigned long long)g_nrec, (double)g_stream_pos / (1024.0 * 1024.0),
            g_truncated ? "  (TRUNCATED)" : "");
    ps2_log("cap: dictionary %llu hits / %llu inserts, %.1f MB held, "
            "%.1f MB of payload never written  (%llu disk stalls)",
            (unsigned long long)hits, (unsigned long long)ins,
            (double)held / (1024.0 * 1024.0),
            (double)saved / (1024.0 * 1024.0),
            (unsigned long long)g_stall_blocks);
    ps2_log("cap: replay it with   gsreplay %s --loop 0 --bench", g_path);
    printf("[capture] %s: %u fields, %.1f MB -> %s\n",
           why && *why ? why : "closed", g_index_n,
           (double)g_stream_pos / (1024.0 * 1024.0), g_path);
    fflush(stdout);
    free(g_index);
    g_index = NULL;
    g_index_n = g_index_cap = 0;
    ps2_cap_dict_reset();
    {
        int i;
        pthread_mutex_lock(&g_m);
        for (i = 0; i < g_nfree; i++) free(g_free[i]);
        g_nfree = 0;
        for (i = 0; i < g_nfull; i++) free(g_full[i].p);
        g_nfull = 0;
        pthread_mutex_unlock(&g_m);
        free(g_buf);
        g_buf = NULL;
    }
    g_stopping = 0;
}

void ps2_cap_report(void) {
    if (g_cap_recording)
        ps2_log("cap: a capture was still recording at exit (%u fields, "
                "%.1f MB) -- closing it", g_index_n,
                (double)g_stream_pos / (1024.0 * 1024.0));
    ps2_cap_stop("run ended");
}

#define CAP_MAX_EMPTY 900

void ps2_cap_present(void) {
    if (!g_cap_recording) return;
    if (g_stop_req) { g_stop_req = 0; ps2_cap_stop("stopped from F6"); return; }
    if (g_nrec == g_frame_nrec) {
        if (++g_empty >= CAP_MAX_EMPTY) {
            g_truncated = 1;
            ps2_cap_stop("gave up: the guest drew nothing for 900 presents");
        }
        return;
    }
    g_empty = 0;
    {
        u64 prims, pixels, regs, trx, tpix;
        ps2_gs_stats(&prims, &pixels, &regs);
        ps2_gs_trx_stats(&trx, &tpix);
        if (cap_begin(PS2_CAP_OP_STATS, 24)) {
            cap_u64(regs - g_last_regs);
            cap_u64(prims - g_last_prims);
            cap_u64(trx - g_last_trx);
        }
        g_last_regs = regs;
        g_last_prims = prims;
        g_last_trx = trx;
    }
    cap_index_push(g_first_field + (u32)g_frames_done);
    if (cap_begin(PS2_CAP_OP_FIELD, 8)) {
        cap_u32(g_first_field + (u32)g_frames_done);
        cap_u32(ps2_gs_field_parity());
    }
    g_frame_nrec = g_nrec;
    g_frames_done++;
    if (!(g_frames_done & 0x3Fu))
        ps2_log("cap: %d fields, %.1f MB", g_frames_done,
                (double)g_stream_pos / (1024.0 * 1024.0));
    if (g_frames_left > 0 && --g_frames_left == 0)
        ps2_cap_stop("frame count reached");
}

void ps2_cap_request_ex(const char *path, int frames, int level) {
    if (g_cap_recording || g_cap_disable || g_req_pending) return;
    if (frames < 0) frames = 0;
    g_reqframes = frames;
    g_reqlevel = level;
    snprintf(g_reqpath, sizeof g_reqpath, "%s", path);
    g_req_pending = 1;
}

void ps2_cap_request(const char *path, int frames) {
    ps2_cap_request_ex(path, frames, PS2_CAP_LEVEL_GS);
}

void ps2_cap_toggle(void) {
    if (g_cap_recording) { g_stop_req = 1; return; }
    ps2_cap_request_ex(g_cfg_path, g_cfg_frames, g_cfg_level);
    ps2_log("cap: capture armed -- %s, %s, level %s", g_cfg_path,
            g_cfg_frames ? "frame-limited" : "until stopped",
            g_cfg_level == PS2_CAP_LEVEL_DMA ? "dma" : "gs");
}

static int cap_env_int(const char *name, int dflt) {
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    return (int)strtol(e, NULL, 0);
}

static void cap_write_ram(void) {
    u32 bmbytes, b, nz = 0;
    u8 *bm;
    bmbytes = (g_hdr.nblk + 7u) / 8u;
    if (!bmbytes) return;
    bm = (u8 *)calloc(bmbytes, 1);
    if (!bm) return;
    for (b = 0; b < g_hdr.nblk; b++) {
        const u64 *p = (const u64 *)(ps2_ram + ((u64)b << g_hdr.blk_shift));
        u32 i;
        for (i = 0; i < (1u << 16) / 8u; i++)
            if (p[i]) { bm[b >> 3] |= (u8)(1u << (b & 7u)); break; }
    }
    fwrite(bm, 1, bmbytes, g_cf);
    for (b = 0; b < g_hdr.nblk; b++)
        if (bm[b >> 3] & (1u << (b & 7u))) {
            fwrite(ps2_ram + ((u64)b << g_hdr.blk_shift), 1, 1u << 16, g_cf);
            nz++;
        }
    free(bm);
    ps2_log("cap: guest RAM %u/%u blocks carried", nz, g_hdr.nblk);
}

void ps2_cap_maybe_start(void) {
    ps2_gs_capstate gst;
    ps2_vu_capstate vst;
    ps2_gif_capstate fst;
    const u8 *vram;
    u32 vram_size = 0;
    int frames, level;
    int i;

    if (g_cap_recording || g_cap_disable) return;
    if (g_arm_field && !g_armed_fired && !g_req_pending
        && ps2_gs_frame_count >= g_arm_field) {
        g_armed_fired = 1;
        ps2_cap_request_ex(g_cfg_path, g_cfg_frames, g_cfg_level);
    }
    if (g_req_pending) {
        snprintf(g_path, sizeof g_path, "%s", g_reqpath);
        frames = g_reqframes;
        level = g_reqlevel;
        g_req_pending = 0;
    } else {
        FILE *mk = fopen("capture.on", "rb");
        char buf[64];
        int marked = 0;
        if (!mk) return;
        frames = g_cfg_frames;
        level = g_cfg_level;
        if (fgets(buf, sizeof buf, mk) && buf[0] >= '0' && buf[0] <= '9') {
            frames = atoi(buf);
            marked = 1;
        }
        if (fgets(buf, sizeof buf, mk)) {
            if (!strncmp(buf, "gs", 2)) level = PS2_CAP_LEVEL_GS;
            else if (!strncmp(buf, "dma", 3)) level = PS2_CAP_LEVEL_DMA;
        }
        fclose(mk);
        remove("capture.on");
        if (!marked && frames == 1) frames = 2;
        ps2_mkdir_p("out");
        snprintf(g_path, sizeof g_path, "%s", g_cfg_path);
    }

    {   const char *slash = strrchr(g_path, '/');
        if (slash && slash - g_path < 400) {
            char dir[400];
            size_t n = (size_t)(slash - g_path);
            memcpy(dir, g_path, n);
            dir[n] = 0;
            ps2_mkdir_p(dir);
        } }

    g_cf = fopen(g_path, "wb");
    if (!g_cf) { ps2_log("cap: cannot open %s", g_path); return; }

    ps2_cap_dict_config((u32)cap_env_int("PS2_CAP_DICT_SLOTS", 0),
                        (u32)cap_env_int("PS2_CAP_DICT_MIN", 0),
                        (u32)cap_env_int("PS2_CAP_DICT_MAX", 0),
                        (u64)cap_env_int("PS2_CAP_DICT_MB", 256) << 20);
    g_max_bytes = (u64)cap_env_int("PS2_CAP_MAX_MB", 16384) << 20;

    vram = ps2_gs_vram_ptr(&vram_size);
    ps2_gs_cap_save(&gst);
    ps2_vu_cap_save(&vst);
    ps2_gif_cap_save(&fst);

    memset(&g_hdr, 0, sizeof g_hdr);
    memcpy(g_hdr.magic, PS2_CAP_MAGIC, 6);
    g_hdr.version = PS2_CAP_VERSION;
    g_hdr.level = (u32)level;
    g_hdr.gs_state_size = (u32)sizeof gst;
    g_hdr.vu_state_size = (u32)sizeof vst;
    g_hdr.gif_state_size = (u32)sizeof fst;
    g_hdr.vram_size = vram ? vram_size : 0u;
    g_hdr.spr_size = ps2_spr ? (u32)PS2_SPR_SIZE : 0u;
    ps2_vu_cap_mem(1, &g_hdr.vu1_mem_size);
    ps2_vu_cap_micro(1, &g_hdr.vu1_micro_size);
    ps2_vu_cap_mem(0, &g_hdr.vu0_mem_size);
    ps2_vu_cap_micro(0, &g_hdr.vu0_micro_size);
    g_hdr.mem_size = ps2_ram ? (u64)PS2_RAM_SIZE : 0u;
    g_hdr.blk_shift = 16;
    g_hdr.nblk = (u32)(g_hdr.mem_size >> g_hdr.blk_shift);
    if (PS2_ENV("PS2_CAP_NORAM")) { g_hdr.mem_size = 0; g_hdr.nblk = 0; }
    g_hdr.dict_slots = dict_slots;
    g_hdr.dict_min = dict_min;
    g_hdr.dict_max = dict_max;
    g_hdr.dict_budget = dict_budget;
    g_hdr.first_field = gst.frame_count;
    fwrite(&g_hdr, sizeof g_hdr, 1, g_cf);

    fwrite(&gst, sizeof gst, 1, g_cf);
    fwrite(&vst, sizeof vst, 1, g_cf);
    {
        u32 n;
        u8 *p;
        p = ps2_vu_cap_mem(1, &n);   if (p && n) fwrite(p, 1, n, g_cf);
        p = ps2_vu_cap_micro(1, &n); if (p && n) fwrite(p, 1, n, g_cf);
        p = ps2_vu_cap_mem(0, &n);   if (p && n) fwrite(p, 1, n, g_cf);
        p = ps2_vu_cap_micro(0, &n); if (p && n) fwrite(p, 1, n, g_cf);
    }
    fwrite(&fst, sizeof fst, 1, g_cf);
    if (g_hdr.vram_size) fwrite(vram, 1, g_hdr.vram_size, g_cf);
    if (g_hdr.spr_size)  fwrite(ps2_spr, 1, g_hdr.spr_size, g_cf);
    if (g_hdr.nblk) cap_write_ram();

    g_stream_off = (u64)ftell(g_cf);
    g_hdr.stream_off = g_stream_off;

    g_nfree = g_nfull = 0;
    for (i = 0; i < CAP_BUFS - 1; i++) {
        u8 *b = (u8 *)malloc(CAP_BUFSZ);
        if (b) g_free[g_nfree++] = b;
    }
    g_buf = (u8 *)malloc(CAP_BUFSZ);
    if (!g_buf) {
        ps2_log("cap: cannot allocate the capture ring");
        fclose(g_cf); g_cf = NULL;
        return;
    }
    g_buf_n = 0;
    g_run_pos = CAP_NORUN;
    g_writer_stop = 0;
    g_writer_live = pthread_create(&g_writer, NULL, cap_writer_main, NULL) == 0;
    if (!g_writer_live)
        ps2_log("cap: no writer thread; the guest will do the I/O itself");

    g_nrec = g_frame_nrec = 0;
    g_stream_pos = 0;
    g_bytes_written = 0;
    g_stall_blocks = 0;
    g_empty = 0;
    g_truncated = 0;
    g_frames_done = 0;
    g_index_n = 0;
    g_first_field = gst.frame_count;
    g_wall0 = ps2_wall_seconds();
    g_frames_left = frames > 0 ? frames : -1;
    {
        u64 prims, pixels, regs, trx, tpix;
        ps2_gs_stats(&prims, &pixels, &regs);
        ps2_gs_trx_stats(&trx, &tpix);
        g_last_regs = regs; g_last_prims = prims; g_last_trx = trx;
    }
    g_cap_recording = 1;
    g_cap_deep = level == PS2_CAP_LEVEL_DMA;
    g_cap_shallow = level == PS2_CAP_LEVEL_GS;

    ps2_log("cap: recording %s at level %s -- %u KB VRAM, %u KB VU, "
            "%s, field %u",
            g_path, g_cap_deep ? "dma (the whole graphics machine)"
                               : "gs (the renderer only)",
            g_hdr.vram_size / 1024u,
            (g_hdr.vu1_mem_size + g_hdr.vu1_micro_size
             + g_hdr.vu0_mem_size + g_hdr.vu0_micro_size) / 1024u,
            frames > 0 ? "frame-limited" : "until stopped", g_first_field);
    printf("[capture] recording -> %s (%s)\n", g_path,
           g_cap_deep ? "dma" : "gs");
    fflush(stdout);
}
