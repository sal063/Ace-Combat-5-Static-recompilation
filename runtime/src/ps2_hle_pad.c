#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <pthread.h>

u64 ps2_kernel_vblank_count(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_PAD2_BUTTON_DATA_SIZE    18
#define SCE_PAD2_BUTTON_PROFILE_SIZE 4
#define scePad2StateNoLink 0
#define scePad2StateStable 1

enum {
    PAD_SELECT = 0, PAD_L3, PAD_R3, PAD_START,
    PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT,
    PAD_L2, PAD_R2, PAD_L1, PAD_R1,
    PAD_TRIANGLE, PAD_CIRCLE, PAD_CROSS, PAD_SQUARE
};

ps2_pad_state ps2_pad_host[PS2_PAD_PORTS];

static ps2_pad_state pad_poll[PS2_PAD_PORTS];
static u16 pad_seen[PS2_PAD_PORTS];
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 pad_latches;

enum { PAD_PATH_BOTH = 0, PAD_PATH_LIBPAD2, PAD_PATH_DBC };
static int pad_path(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("PS2_PAD_PATH");
        cached = PAD_PATH_LIBPAD2;
        if (e && !strcmp(e, "libpad2")) cached = PAD_PATH_LIBPAD2;
        else if (e && !strcmp(e, "dbc")) cached = PAD_PATH_DBC;
        else if (e && !strcmp(e, "both")) cached = PAD_PATH_BOTH;
        if (cached != PAD_PATH_BOTH)
            ps2_log("pad: only the %s path will report input",
                    cached == PAD_PATH_LIBPAD2 ? "libpad2" : "DBCMAN");
    }
    return cached;
}

void ps2_pad_publish(int port, const ps2_pad_state *st) {
    if (port < 0 || port >= PS2_PAD_PORTS) return;
    pthread_mutex_lock(&pad_lock);
    pad_poll[port] = *st;
    pad_seen[port] |= st->buttons;
    pthread_mutex_unlock(&pad_lock);
}

static FILE *pad_rec_fp;
static FILE *pad_play_fp;
static ps2_pad_state *pad_play_buf;
static u64 pad_play_n, pad_play_used;
static int pad_io_ready;

#define PAD_REC_MAGIC "PS2PAD1\0"

static void pad_io_init(void) {
    const char *rec, *play;
    if (pad_io_ready) return;
    pad_io_ready = 1;
    rec = getenv("PS2_PAD_RECORD");
    play = getenv("PS2_PAD_PLAY");
    if (rec && *rec) {
        pad_rec_fp = fopen(rec, "wb");
        if (pad_rec_fp) {
            fwrite(PAD_REC_MAGIC, 1, 8, pad_rec_fp);
            ps2_log("pad: recording input to %s", rec);
        } else ps2_log("pad: cannot open %s for recording", rec);
    }
    if (play && *play) {
        char magic[8];
        pad_play_fp = fopen(play, "rb");
        if (!pad_play_fp) { ps2_log("pad: cannot open %s", play); return; }
        if (fread(magic, 1, 8, pad_play_fp) != 8
            || memcmp(magic, PAD_REC_MAGIC, 8)) {
            ps2_log("pad: %s is not an input recording", play);
            fclose(pad_play_fp); pad_play_fp = NULL; return;
        }
        {   long here = ftell(pad_play_fp), end;
            fseek(pad_play_fp, 0, SEEK_END);
            end = ftell(pad_play_fp);
            fseek(pad_play_fp, here, SEEK_SET);
            pad_play_n = (u64)(end - here)
                       / (sizeof(ps2_pad_state) * PS2_PAD_PORTS);
        }
        pad_play_buf = (ps2_pad_state *)malloc(
            (size_t)pad_play_n * sizeof(ps2_pad_state) * PS2_PAD_PORTS);
        if (!pad_play_buf
            || fread(pad_play_buf, sizeof(ps2_pad_state) * PS2_PAD_PORTS,
                     (size_t)pad_play_n, pad_play_fp) != pad_play_n) {
            ps2_log("pad: short read on %s", play);
            free(pad_play_buf); pad_play_buf = NULL; pad_play_n = 0;
        } else {
            ps2_log("pad: replaying %llu fields of input from %s",
                    (unsigned long long)pad_play_n, play);
        }
        fclose(pad_play_fp);
        pad_play_fp = NULL;
    }
}

void ps2_pad_io_report(void) {
    if (pad_play_n)
        ps2_log("pad: input replay used %llu of %llu recorded fields%s",
                (unsigned long long)pad_play_used,
                (unsigned long long)pad_play_n,
                pad_play_used < pad_play_n
                    ? "  (the run was shorter than the recording)" : "");
    if (pad_rec_fp) {
        ps2_log("pad: recorded %llu fields of input",
                (unsigned long long)pad_latches);
        fclose(pad_rec_fp);
        pad_rec_fp = NULL;
    }
}

void ps2_pad_latch(void) {
    int i;
    pthread_mutex_lock(&pad_lock);
    for (i = 0; i < PS2_PAD_PORTS; i++) {
        ps2_pad_host[i] = pad_poll[i];
        ps2_pad_host[i].buttons |= pad_seen[i];
        pad_seen[i] = pad_poll[i].buttons;
    }
    pad_latches++;
    pthread_mutex_unlock(&pad_lock);
    if (!pad_io_ready) pad_io_init();
    if (pad_play_buf && pad_play_used < pad_play_n) {
        memcpy(ps2_pad_host,
               pad_play_buf + pad_play_used * PS2_PAD_PORTS,
               sizeof(ps2_pad_state) * PS2_PAD_PORTS);
        pad_play_used++;
        if (pad_play_used == pad_play_n) {
            const char *e = getenv("PS2_PAD_PLAY_STOP");
            if (!(e && *e == '0')) ps2_finish("input recording ended");
        }
    }
    if (pad_rec_fp)
        fwrite(ps2_pad_host, sizeof(ps2_pad_state), PS2_PAD_PORTS, pad_rec_fp);
    if (PS2_ENV("PS2_PAD_DIAG")) {
        static u16 last_buttons;
        static int last_connected = -1;
        const ps2_pad_state *h = &ps2_pad_host[0];
        if (h->buttons != last_buttons || h->connected != last_connected) {
            ps2_log("pad-buttons: field=%llu connected=%d buttons=%04X LT=%u RT=%u",
                (unsigned long long)pad_latches, h->connected, h->buttons, h->l2, h->r2);
            last_buttons = h->buttons; last_connected = h->connected;
        }
    }
    if (PS2_ENV("PS2_TRACE_PAD")) {
        static ps2_pad_state last;
        static int have_last;
        const ps2_pad_state *h = &ps2_pad_host[0];
        if (!have_last || memcmp(&last, h, sizeof(last)) != 0) {
            have_last = 1;
            last = *h;
            ps2_log("pad latch %llu: buttons=%04X  L(%u,%u) R(%u,%u) "
                    "LT=%u RT=%u", (unsigned long long)pad_latches,
                    h->buttons, h->lx, h->ly, h->rx, h->ry, h->l2, h->r2);
        }
    }
}

typedef struct {
    int used;
    int port, slot;
} pad_socket;

#define MAX_SOCKETS 8
static pad_socket sockets[MAX_SOCKETS];
static int pad_inited;
static u64 pad_reads;
static u64 dbc_calls;

void ps2_pad_init(void) {
    memset(sockets, 0, sizeof(sockets));
    memset(ps2_pad_host, 0, sizeof(ps2_pad_host));
    memset(pad_seen, 0, sizeof(pad_seen));
    for (int i = 0; i < PS2_PAD_PORTS; i++) {
        ps2_pad_host[i].lx = ps2_pad_host[i].ly = PS2_PAD_ANALOG_NEUTRAL;
        ps2_pad_host[i].rx = ps2_pad_host[i].ry = PS2_PAD_ANALOG_NEUTRAL;
    }
    ps2_pad_host[0].connected = 1;
    memcpy(pad_poll, ps2_pad_host, sizeof(pad_poll));
    pad_inited = 0;
    pad_reads = 0;
    dbc_calls = 0;
    pad_latches = 0;
}

void hle_scePad2Init(ps2_ctx *ctx) {
    if (!pad_inited) { pad_inited = 1; ps2_log("pad: libpad2 serviced natively"); }
    HRET(0);
}
void hle_scePad2End(ps2_ctx *ctx) { pad_inited = 0; HRET(0); }

void hle_scePad2CreateSocket(ps2_ctx *ctx) {
    u32 p = ps2_arg(ctx, 0);
    int port = p ? (int)ps2_r32(p + 4) : 0;
    int slot = p ? (int)ps2_r32(p + 8) : 0;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].used) continue;
        sockets[i].used = 1;
        sockets[i].port = port;
        sockets[i].slot = slot;
        ps2_log("pad: socket %d -> port %d slot %d", i, port, slot);
        HRET(i);
        return;
    }
    HRET(-1);
}

void hle_scePad2DeleteSocket(ps2_ctx *ctx) {
    int s = (int)ps2_arg(ctx, 0);
    if (s >= 0 && s < MAX_SOCKETS) sockets[s].used = 0;
    HRET(0);
}

static const ps2_pad_state *state_for(int socket) {
    int port = 0;
    if (socket >= 0 && socket < MAX_SOCKETS && sockets[socket].used)
        port = sockets[socket].port;
    if (port < 0 || port >= PS2_PAD_PORTS) port = 0;
    return &ps2_pad_host[port];
}

void hle_scePad2GetState(ps2_ctx *ctx) {
    const ps2_pad_state *st = state_for((int)ps2_arg(ctx, 0));
    if (pad_path() == PAD_PATH_DBC) { HRET(scePad2StateNoLink); return; }
    HRET(st->connected ? scePad2StateStable : scePad2StateNoLink);
}

static int pad_autoplay;
void ps2_pad_autoplay(int on) { pad_autoplay = on; }

#define PAD_SCRIPT_MAX 32
static struct { u64 field; u16 mask; } pad_script[PAD_SCRIPT_MAX];
static unsigned pad_script_n;
static int pad_script_parsed;

static u16 pad_name_to_mask(const char *n, size_t len) {
    static const char *nm[16] = {
        "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
        "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"
    };
    for (int i = 0; i < 16; i++)
        if (strlen(nm[i]) == len && !strncmp(nm[i], n, len)) return 1u << i;
    if (len && n[0] >= '0' && n[0] <= '9')
        return (u16)(1u << (strtoul(n, NULL, 0) & 15u));
    return 0;
}

static void pad_script_parse(void) {
    const char *e = getenv("PS2_PAD_SCRIPT");
    const char *p;
    pad_script_parsed = 1;
    if (!e) return;
    for (p = e; *p; ) {
        char *end;
        u64 field;
        const char *name;
        u16 mask;
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        field = strtoull(p, &end, 0);
        if (end == p || *end != ':') break;
        name = end + 1;
        p = name;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') p++;
        mask = pad_name_to_mask(name, (size_t)(p - name));
        if (mask && pad_script_n < PAD_SCRIPT_MAX) {
            pad_script[pad_script_n].field = field;
            pad_script[pad_script_n].mask = mask;
            pad_script_n++;
        }
    }
    if (pad_script_n)
        ps2_log("pad: %u scripted presses", pad_script_n);
}

static u16 pad_buttons(int port, const ps2_pad_state *st) {
    u16 buttons = st->buttons;
    {
        const char *h = getenv("PS2_HOLD");
        if (h && port == 0) buttons |= 1u << (strtoul(h, NULL, 0) & 15u);
    }
    if (port == 0) {
        u64 now = ps2_kernel_vblank_count();
        if (!pad_script_parsed) pad_script_parse();
        for (unsigned i = 0; i < pad_script_n; i++)
            if (now >= pad_script[i].field && now < pad_script[i].field + 8u)
                buttons |= pad_script[i].mask;
    }
    if (pad_autoplay && port == 0 && pad_reads > 240) {
        u64 phase = pad_reads % 150;
        if (phase < 6)       buttons |= 1u << PAD_CROSS;
        else if (phase < 12) buttons |= 1u << PAD_START;
        else if (phase < 18) buttons |= 1u << PAD_CIRCLE;
    }
    return buttons;
}

static int port_of(int socket) {
    if (socket >= 0 && socket < MAX_SOCKETS && sockets[socket].used) {
        int p = sockets[socket].port;
        if (p >= 0 && p < PS2_PAD_PORTS) return p;
    }
    return 0;
}

void hle_scePad2Read(ps2_ctx *ctx) {
    const ps2_pad_state *st = state_for((int)ps2_arg(ctx, 0));
    u32 buf = ps2_arg(ctx, 1);
    u16 buttons;
    u16 digital;
    u8 d[SCE_PAD2_BUTTON_DATA_SIZE];

    pad_reads++;
    buttons = pad_buttons(port_of((int)ps2_arg(ctx, 0)), st);
    if (pad_path() == PAD_PATH_DBC) buttons = 0;
    if (ps2_verbose) {
        static u16 last[PS2_PAD_PORTS];
        int port = port_of((int)ps2_arg(ctx, 0));
        if (buttons != last[port]) {
            static const char *nm[16] = {
                "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"
            };
            char line[160];
            int n = 0, b;
            last[port] = buttons;
            for (b = 0; b < 16; b++)
                if (buttons & (1u << b))
                    n += snprintf(line + n, sizeof line - (size_t)n, " %s", nm[b]);
            ps2_log("pad: port %d buttons %04X%s  sticks L(%u,%u) R(%u,%u)",
                    port, buttons, n ? line : " (none)",
                    st->lx, st->ly, st->rx, st->ry);
        }
    }
    digital = (u16)~buttons;
    memset(d, 0, sizeof(d));
    d[0] = (u8)(digital & 0xFF);
    d[1] = (u8)(digital >> 8);
    d[2] = st->rx;
    d[3] = st->ry;
    d[4] = st->lx;
    d[5] = st->ly;
    d[6]  = (buttons >> PAD_RIGHT)    & 1 ? 255 : 0;
    d[7]  = (buttons >> PAD_LEFT)     & 1 ? 255 : 0;
    d[8]  = (buttons >> PAD_UP)       & 1 ? 255 : 0;
    d[9]  = (buttons >> PAD_DOWN)     & 1 ? 255 : 0;
    d[10] = (buttons >> PAD_TRIANGLE) & 1 ? 255 : 0;
    d[11] = (buttons >> PAD_CIRCLE)   & 1 ? 255 : 0;
    d[12] = (buttons >> PAD_CROSS)    & 1 ? 255 : 0;
    d[13] = (buttons >> PAD_SQUARE)   & 1 ? 255 : 0;
    d[14] = (buttons >> PAD_L1)       & 1 ? 255 : 0;
    d[15] = (buttons >> PAD_R1)       & 1 ? 255 : 0;
    d[16] = (buttons >> PAD_L2) & 1 ? ((st->buttons >> PAD_L2) & 1 ? st->l2 : 255) : 0;
    d[17] = (buttons >> PAD_R2) & 1 ? ((st->buttons >> PAD_R2) & 1 ? st->r2 : 255) : 0;
    if (buf) ps2_put_mem(buf, d, sizeof(d));
    if (buf && PS2_ENV("PS2_TRACE_PAD")) {
        u32 obj = buf - 628u;
        static u32 last_held = 0xFFFFFFFFu;
        u32 held = ps2_r16(obj + 916), edge = ps2_r16(obj + 918);
        if (held != last_held) {
            last_held = held;
            ps2_log("pad decode: obj=%08X held=%04X edge=%04X notfull=%u "
                    "profile=%02X%02X%02X%02X state=%d",
                    obj, held, edge, ps2_r8(obj + 920),
                    ps2_r8(obj + 596), ps2_r8(obj + 597),
                    ps2_r8(obj + 598), ps2_r8(obj + 599),
                    (int)ps2_r32(obj + 620));
        }
    }
    HRET(SCE_PAD2_BUTTON_DATA_SIZE);
}

void hle_scePad2GetButtonProfile(ps2_ctx *ctx) {
    u32 buf = ps2_arg(ctx, 1);
    u8 p[SCE_PAD2_BUTTON_PROFILE_SIZE] = { 0xFF, 0xFF, 0xFF, 0xFF };
    if (buf) ps2_put_mem(buf, p, sizeof(p));
    HRET(SCE_PAD2_BUTTON_PROFILE_SIZE);
}

void hle_scePad2InitDmaDBuff(ps2_ctx *ctx)   { HRET(0); }
void hle_scePad2LinkDriver(ps2_ctx *ctx)     { HRET(0); }
void hle_scePad2CheckDma(ps2_ctx *ctx)       { HRET(0); }
void hle_scePad2SetButtonOrder(ps2_ctx *ctx) { HRET(0); }
void hle_scePad2GetSide(ps2_ctx *ctx)        { HRET(0); }
void hle_scePad2GetSide2(ps2_ctx *ctx)       { HRET(0); }

void hle_sceVibGetProfile(ps2_ctx *ctx) {
    const ps2_pad_state *st = state_for((int)ps2_arg(ctx, 0));
    u32 buf = ps2_arg(ctx, 1);
    if (!st->connected) { HRET(-1); return; }
    if (buf) ps2_w8(buf, 3);
    HRET(1);
}

#define DBC_DEV0            8u
#define DBC_DEV_STRIDE      48u
#define DBC_KIND_PLAYER1    2u

static const u32 dbc_slot_of[8] = { 0, 1, 2, 3, 4, 5, 7, 6 };

static u32 dbc_raw(u32 obj, int j, int out) {
    int lo = (int)ps2_r8(obj + 341u + (u32)j);
    int hi = (int)ps2_r8(obj + 357u + (u32)j);
    int raw;
    if (hi <= lo) return (u32)(lo > 0 ? lo - 1 : 0);
    raw = lo + (out * (hi - lo) + 127) / 255;
    if (raw < lo) raw = lo;
    if (raw > hi) raw = hi;
    return (u32)raw;
}

static int dbc_axis(int neg, int pos) {
    int v = 128 + (pos - neg) / 2;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

static void dbc_trace(u32 obj) {
    u32 mgr = obj + 0x330u, inp = obj + 0x47Cu;
    u32 n = ps2_r8(mgr + 328u);
    static u32 last_edge = 0xFFFFFFFFu;
    static u32 last_n = 0xFFFFFFFFu;
    static u32 last_held = 0xFFFFFFFFu;
    u32 edge = n ? ps2_r32(mgr + 64u) : 0;
    u32 held = n ? ps2_r32(mgr + 36u) : 0;
    if (edge == last_edge && n == last_n && held == last_held) return;
    last_edge = edge;
    last_n = n;
    last_held = held;
    ps2_log("dbc: %u device(s), rec0 type=%u cfg=%u held=%08X new=%08X, "
            "player1 assign=%d", n,
            n ? ps2_r8(mgr + 44u) : 0, n ? ps2_r8(mgr + 46u) : 0,
            n ? ps2_r32(mgr + 36u) : 0, edge, (int)(s8)ps2_r8(inp + 116u));
}

static void dbc_trace_record(u32 obj) {
    u32 mgr = obj + 0x330u;
    { static u32 said; if (said != mgr) { said = mgr;
        ps2_log("dbc: pad manager at %08X, record 0 at %08X "
                "(prev-field masks at %08X)", mgr, mgr, mgr + 56u); } }
    u32 n = ps2_r8(mgr + 328u);
    u32 cur = ps2_r32(mgr + 36u), cur2 = ps2_r32(mgr + 40u);
    u32 prev = ps2_r32(mgr + 56u), prev2 = ps2_r32(mgr + 60u);
    u32 nw = ps2_r32(mgr + 64u), nw2 = ps2_r32(mgr + 68u);
    u32 rep = ps2_r32(mgr + 72u), rep2 = ps2_r32(mgr + 76u);
    static int quiet;
    if (!n) return;
    if (!(cur | cur2 | prev | prev2 | nw | nw2 | rep | rep2)) {
        if (++quiet > 2) return;
    } else {
        quiet = 0;
    }
    ps2_log("dbc rec: cur=%08X/%08X prev=%08X/%08X new=%08X/%08X "
            "rep=%08X/%08X  delay=%u period=%u count=%u",
            cur, cur2, prev, prev2, nw, nw2, rep, rep2,
            ps2_r8(mgr + 48u), ps2_r8(mgr + 49u), ps2_r8(mgr + 50u));
    {
        u32 k;
        for (k = 0; k < 3 && k < n + 1u; k++) {
            u32 r = mgr + 80u * k;
            ps2_log("   slot %u: type=%u sub=%u flag=%u cur=%08X prev=%08X "
                    "new=%08X", k, ps2_r8(r + 44u), ps2_r8(r + 45u),
                    ps2_r8(r + 47u), ps2_r32(r + 36u), ps2_r32(r + 56u),
                    ps2_r32(r + 64u));
        }
    }
}

void ps2_dbc_report(u32 obj, u32 recv, int rsize) {
    const ps2_pad_state *st = &ps2_pad_host[0];
    u16 buttons;
    u32 dig;
    int out[8];
    int l1, r1, l2, r2, tri, sqr, j;

    if (rsize < (int)(DBC_DEV0 + DBC_DEV_STRIDE)) return;
    dbc_calls++;
    buttons = pad_buttons(0, st);
    if (pad_path() == PAD_PATH_LIBPAD2) {
        ps2_w32(recv + 0u, 0);
        ps2_w32(recv + 4u, 0);
        return;
    }
    if (!st->connected) {
        ps2_w32(recv + 0u, 0);
        ps2_w32(recv + 4u, 0);
        return;
    }
    ps2_w32(recv + 0u, DBC_KIND_PLAYER1);
    ps2_w32(recv + 4u, 0);

    dig = 0xFFFF0000u | (u32)(u16)~buttons;
    ps2_w32(recv + DBC_DEV0 + 32u, (dig >>  0) & 0xFFu);
    ps2_w32(recv + DBC_DEV0 + 36u, (dig >>  8) & 0xFFu);
    ps2_w32(recv + DBC_DEV0 + 40u, (dig >> 16) & 0xFFu);
    ps2_w32(recv + DBC_DEV0 + 44u, (dig >> 24) & 0xFFu);

    l1  = (buttons >> PAD_L1)       & 1 ? 255 : 0;
    r1  = (buttons >> PAD_R1)       & 1 ? 255 : 0;
    l2  = (buttons >> PAD_L2)       & 1 ? 255 : st->l2;
    r2  = (buttons >> PAD_R2)       & 1 ? 255 : st->r2;
    tri = (buttons >> PAD_TRIANGLE) & 1 ? 255 : 0;
    sqr = (buttons >> PAD_SQUARE)   & 1 ? 255 : 0;

    out[0] = st->lx;
    out[1] = st->ly;
    out[2] = dbc_axis(l2, r2);
    out[3] = dbc_axis(l1, r1);
    out[4] = st->rx;
    out[5] = st->ry;
    out[6] = tri;
    out[7] = sqr;

    for (j = 0; j < 8; j++)
        ps2_w32(recv + DBC_DEV0 + 4u * dbc_slot_of[j],
                dbc_raw(obj, j, out[j]));

    if (PS2_ENV("PS2_TRACE_PAD")) {
        const char *lv = getenv("PS2_TRACE_PAD");
        dbc_trace(obj);
        if (lv && lv[0] >= '2') dbc_trace_record(obj);
    }
}

void ps2_pad_latch(void);

void ps2_pad_report(void) {
    ps2_log("pad: %llu libpad2 reads, %llu DBCMAN device reports",
            (unsigned long long)pad_reads, (unsigned long long)dbc_calls);
}
