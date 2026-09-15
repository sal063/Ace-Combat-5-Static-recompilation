#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps2_nufile_register(void);
u64  ps2_kernel_vblank_count(void);
void ps2_nusound_register(void);
void ps2_spu2_key_on(u32 i, u32 ssa, u32 lsax, u32 pitch,
                     u32 adsr1, u32 adsr2, s16 voll, s16 volr);
void ps2_spu2_key_off(u32 i);
void ps2_spu2_stream_volume(u32 i, u32 l, u32 r);
void ps2_spu2_set_voice(u32 i, u32 ssa, u32 pitch, u32 adsr1, u32 adsr2,
                        u32 voll_reg, u32 volr_reg, int loop);
void ps2_spu2_key_on_mask(u32 core0, u32 core1);
void ps2_spu2_key_off_mask(u32 core0, u32 core1);
extern u8 *ps2_spu2_ram;
#define PS2_SPU2_RAM_SIZE  (2u * 1024u * 1024u)
void ps2_nusndstr_register(void);
void ps2_spu2_write(u32 dst, u32 iop_src, u32 size);

static void zero_reply(u32 recv, int rsize) {
    if (recv && rsize > 0)
        for (int i = 0; i < rsize; i += 4) ps2_w32(recv + (u32)i, 0);
}

#define LGDEV_SID       0x046D046Du
#define LGDEV_FNO_ENUM  1
#define LGDEV_FNO_INIT  12
#define LGDEV_FNO_CONF  18
#define LGDEV_VERSION   0x108u
#define LGDEV_ERR_NO_MORE_DEVICES 0x80000002u

static int lgdev_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                     u32 recv, int rsize) {
    (void)ctx; (void)send; (void)ssize;
    zero_reply(recv, rsize);
    if (fno == LGDEV_FNO_ENUM && rsize >= 4) {
        static int said;
        ps2_w32(recv, LGDEV_ERR_NO_MORE_DEVICES);
        if (!said) {
            said = 1;
            ps2_log("lgdev: enumerate -> no devices attached");
        }
    }
    if (fno == LGDEV_FNO_INIT && rsize >= 8) {
        ps2_w32(recv + 4, LGDEV_VERSION);
        ps2_log("lgdev: reported version %u.%02u", LGDEV_VERSION >> 8,
                LGDEV_VERSION & 0xFF);
    }
    return 0;
}

#define DBC_SID_MAIN    0x80001300u
#define DBC_FNO_VERSION 0x80001363u
#define DBC_VERSION     0x0300u

static int dbc_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                   u32 recv, int rsize) {
    (void)ctx; (void)send; (void)ssize;
    zero_reply(recv, rsize);
    if (fno == DBC_FNO_VERSION && rsize >= 4)
        ps2_w32(recv, DBC_VERSION);
    return 0;
}

#define DEV_SID          0x80000210u
#define DEV_FNO_REPORT   2u

static int dev_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                   u32 recv, int rsize) {
    (void)ctx; (void)ssize;
    zero_reply(recv, rsize);
    if (fno == DEV_FNO_REPORT) ps2_dbc_report(send, recv, rsize);
    return 0;
}

static int ok_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                  u32 recv, int rsize) {
    (void)ctx; (void)fno; (void)send; (void)ssize;
    zero_reply(recv, rsize);
    return 0;
}

#define MCSERV_SID       0x80000400u
#define MCSERV_FNO_INIT  254u
#define MCSERV_VERSION   522u
#define MCMAN_VERSION    526u

static int mcserv_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                      u32 recv, int rsize) {
    extern int ps2_memcard_rpc(ps2_ctx *,u32,u32,int,u32,int);
    return ps2_memcard_rpc(ctx,fno,send,ssize,recv,rsize);
}

void ps2_iop_services_register(void) {
    ps2_rpc_register(LGDEV_SID,   lgdev_rpc, "lgdev");
    ps2_rpc_register(DBC_SID_MAIN, dbc_rpc,  "dbcman");
    ps2_rpc_register(0x8000131Bu, dbc_rpc,   "dbcman-b");
    ps2_rpc_register(0x8000131Cu, dbc_rpc,   "dbcman-c");
    ps2_rpc_register(0x80000001u, ok_rpc,    "fileio");
    ps2_rpc_register(0x80000003u, ok_rpc,    "sysmem");
    ps2_rpc_register(0x80000006u, ok_rpc,    "loadfile");
    ps2_rpc_register(MCSERV_SID,  mcserv_rpc, "mcserv");
    ps2_rpc_register(DEV_SID,     dev_rpc,   "devices");
    ps2_nufile_register();
    ps2_nusound_register();
    ps2_nusndstr_register();
}

void hle_write(ps2_ctx *ctx) {
    static char line[1024];
    static size_t fill;
    int fd = (int)ps2_arg(ctx, 0);
    u32 buf = ps2_arg(ctx, 1);
    int len = (int)ps2_arg(ctx, 2);
    if (fd != 1 && fd != 2) { HRET(-1); return; }
    for (int i = 0; i < len; i++) {
        char c = (char)ps2_r8(buf + (u32)i);
        if (c == '\n' || fill == sizeof(line) - 1) {
            line[fill] = 0;
            if (fill) ps2_log("guest: %s", line);
            fill = 0;
            if (c != '\n') line[fill++] = c;
        } else if (c != '\r') {
            line[fill++] = c;
        }
    }
    HRET(len);
}

#define NUFILE_SID  0x4E554649u

#define NUFILE_MAX_FD 64
typedef struct {
    int used;
    const ps2_disc_file *f;
    u64 pos;
} nufile_fd;

static nufile_fd nufile_fds[NUFILE_MAX_FD];
static u64 nufile_calls, nufile_bytes;
static u32 nufile_seen[64];
static unsigned nufile_nseen;

static int nufile_first(u32 fno) {
    for (unsigned i = 0; i < nufile_nseen; i++)
        if (nufile_seen[i] == fno) return 0;
    if (nufile_nseen < 64) nufile_seen[nufile_nseen++] = fno;
    return 1;
}

static int nufile_rpc_inner(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                            u32 recv, int rsize);

static int nufile_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                      u32 recv, int rsize) {
    u32 cb = 0, cb_sema = 0;
    int rc;
    if (send && ssize >= 8) {
        cb = ps2_r32(send + 0x00);
        cb_sema = ps2_r32(send + 0x04);
    }
    rc = nufile_rpc_inner(ctx, fno, send, ssize, recv, rsize);
    if (ctx && cb) {
        u32 result = (recv && rsize >= 4) ? ps2_r32(recv) : 0u;
        ps2_ctx sub = *ctx;
        sub.r[4].ud[0] = 0;
        sub.r[5].ud[0] = (u64)(s64)(s32)result;
        sub.r[6].ud[0] = (u64)cb_sema;
        ps2_dispatch(&sub, cb);
    }
    return rc;
}

static int nufile_rpc_inner(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                            u32 recv, int rsize) {
    char name[256];
    (void)ctx;
    nufile_calls++;
    switch (fno) {
    case 1: {
        u32 len = ps2_r32(send + 0x18);
        int fd;
        const ps2_disc_file *f;
        if (len > sizeof(name) - 1) len = sizeof(name) - 1;
        for (u32 i = 0; i < len; i++) name[i] = (char)ps2_r8(send + 0x1C + i);
        name[len ? len - 1 : 0] = 0;
        f = ps2_iso_find(name);
        for (fd = 1; fd < NUFILE_MAX_FD; fd++)
            if (!nufile_fds[fd].used) break;
        if (!f || fd >= NUFILE_MAX_FD) {
            ps2_log("nufile: open '%s' -> not found", name);
            ps2_w32(recv, (u32)-1);
            return 0;
        }
        nufile_fds[fd].used = 1;
        nufile_fds[fd].f = f;
        nufile_fds[fd].pos = 0;
        ps2_log("nufile: open '%s' -> fd %d (%u bytes)", name, fd, f->size);
        ps2_w32(recv, (u32)fd);
        return 0;
    }
    case 2: {
        u32 fd = ps2_r32(send + 0x08);
        if (fd < NUFILE_MAX_FD) nufile_fds[fd].used = 0;
        ps2_w32(recv, 0);
        return 0;
    }
    case 3:
    case 4: {
        u32 fd  = ps2_r32(send + 0x08);
        u32 buf = ps2_r32(send + 0x0C);
        u32 len = ps2_r32(send + 0x10);
        int got;
        if (fd >= NUFILE_MAX_FD || !nufile_fds[fd].used) {
            ps2_log("nufile: read on bad handle %u", fd);
            ps2_w32(recv, (u32)-1);
            return 0;
        }
        got = ps2_iso_read_file(nufile_fds[fd].f, nufile_fds[fd].pos, len, buf);
        if (got < 0) got = 0;
        nufile_fds[fd].pos += (u64)got;
        nufile_bytes += (u64)got;
        if (ps2_verbose)
            ps2_log("nufile: read fd %u %u bytes -> %08X (%d, pos now %llu)",
                    fd, len, buf, got,
                    (unsigned long long)nufile_fds[fd].pos);
        ps2_w32(recv, (u32)got);
        return 0;
    }
    case 5: {
        u32 fd     = ps2_r32(send + 0x08);
        u32 off    = ps2_r32(send + 0x0C);
        u32 whence = ps2_r32(send + 0x10);
        u64 size, pos;
        if (fd >= NUFILE_MAX_FD || !nufile_fds[fd].used) {
            ps2_log("nufile: seek on bad handle %u", fd);
            ps2_w32(recv, (u32)-1);
            return 0;
        }
        size = (u64)nufile_fds[fd].f->size;
        if (whence == 1u)      pos = nufile_fds[fd].pos + (u64)off;
        else if (whence == 2u) pos = size + (u64)off;
        else                   pos = (u64)off;
        if (pos > size) pos = size;
        nufile_fds[fd].pos = pos;
        if (ps2_verbose)
            ps2_log("nufile: seek fd %u to %llu (off=%u whence=%u of %llu)",
                    fd, (unsigned long long)pos, off, whence,
                    (unsigned long long)size);
        ps2_w32(recv, (u32)pos);
        return 0;
    }
    case 8: {
        u32 size = ps2_r32(send + 0x08);
        u32 buf  = ps2_r32(send + 0x0C);
        u32 len  = ps2_r32(send + 0x10);
        const ps2_disc_file *f;
        int got;
        if (len > sizeof(name)) len = sizeof(name);
        for (u32 i = 0; i < len; i++) name[i] = (char)ps2_r8(send + 0x14 + i);
        name[len ? len - 1 : 0] = 0;
        f = ps2_iso_find(name);
        if (!f) {
            ps2_log("nufile: load '%s' -> not found", name);
            ps2_w32(recv, (u32)-1);
            return 0;
        }
        got = ps2_iso_read_file(f, 0, size ? size : f->size, buf);
        if (got < 0) got = 0;
        nufile_bytes += (u64)got;
        ps2_log("nufile: load '%s' (%u of %u bytes) -> %08X", name,
                (u32)got, f->size, buf);
        ps2_w32(recv, (u32)got);
        return 0;
    }
    default:
        if (nufile_first(fno)) {
            u32 w[8];
            for (int i = 0; i < 8; i++) w[i] = ps2_r32(send + (u32)i * 4);
            ps2_log("nufile: fno %u (ssize=%d rsize=%d) "
                    "%08X %08X %08X %08X %08X %08X %08X %08X",
                    fno, ssize, rsize, w[0], w[1], w[2], w[3],
                    w[4], w[5], w[6], w[7]);
        }
        zero_reply(recv, rsize);
        return 0;
    }
}

void ps2_nufile_report(void) {
    ps2_log("nufile: %llu calls, %llu bytes read",
            (unsigned long long)nufile_calls, (unsigned long long)nufile_bytes);
}

void ps2_nufile_register(void) {
    ps2_rpc_register(NUFILE_SID, nufile_rpc, "nufile");
}

void hook_file_open(ps2_ctx *ctx) {
    char path[256];
    ps2_get_str(ps2_arg(ctx, 0), path, sizeof(path));
    ps2_log("file: open('%s', %08X, %08X, %08X, %08X)", path,
            ps2_arg(ctx, 1), ps2_arg(ctx, 2), ps2_arg(ctx, 3), ps2_arg(ctx, 4));
}

void hook_sound_load(ps2_ctx *ctx) {
    char path[256];
    ps2_get_str(ps2_arg(ctx, 0), path, sizeof(path));
    ps2_log("sound: load('%s', %08X, %08X) field=%llu caller=%08X", path,
            ps2_arg(ctx, 1), ps2_arg(ctx, 2),
            (unsigned long long)ps2_kernel_vblank_count(), (u32)ctx->r[31].ud[0]);
}

void hook_dev_lookup(ps2_ctx *ctx) {
    char name[64];
    ps2_get_str(ps2_arg(ctx, 0), name, sizeof(name));
    ps2_log("file: device lookup '%s'", name);
}

void hook_path_split(ps2_ctx *ctx) {
    char path[256];
    ps2_get_str(ps2_arg(ctx, 0), path, sizeof(path));
    ps2_log("file: path split '%s'", path);
}

#define NUSOUND_SID 0x000695DDu

static u64 nusound_calls, nusound_cmds;

#define NUSOUND_SLOTS   8
#define NU_ST_INIT      0
#define NU_ST_PLAYWAIT  1
#define NU_ST_KEYONWAIT 3
#define NU_ST_GOWAIT    4
#define NU_ST_PLAYING   5
#include "ps2_nustream.h"
static u32 nusound_state[NUSOUND_SLOTS];
static u8 nusound_started[NUSOUND_SLOTS];
static u8 nusound_wait_go[NUSOUND_SLOTS];
static u32 nusound_seen[256];
static unsigned nusound_nseen;
static char nusound_filename[256];
static int nusound_first(u32 cmd) {
    for (unsigned i = 0; i < nusound_nseen; i++) if (nusound_seen[i] == cmd) return 0;
    if (nusound_nseen < 256) nusound_seen[nusound_nseen++] = cmd;
    return 1;
}
static void nusound_control(u32 cmd, const u32 *a) {
    u32 slot = a[0];
    if (cmd == 2) {
        memset(nusound_state, 0, sizeof nusound_state);
        memset(nusound_started, 0, sizeof nusound_started);
        memset(nusound_wait_go, 0, sizeof nusound_wait_go);
    } else if (cmd == 12) {
        for (u32 i = 0; i < 8; i++) if (slot & (1u << i)) {
            nusound_wait_go[i] = 0;
            if (nusound_state[i] == NU_ST_GOWAIT) {
                nusound_state[i] = NU_ST_KEYONWAIT;
                nusound_started[i] = 1;
                ps2_log("nusound: stream %u Go Wait -> KeyOnWait (mask %08X field=%llu)",
                        i, slot, (unsigned long long)ps2_kernel_vblank_count());
            }
        }
    } else if (slot < 8) {
        u32 before = nusound_state[slot];
        switch (cmd) {
        case 14: nusound_wait_go[slot] = (u8)a[1] != 0; break;
        case 7:
            nusound_started[slot] = !nusound_wait_go[slot];
            nusound_state[slot] = nusound_wait_go[slot] ? NU_ST_GOWAIT : NU_ST_KEYONWAIT;
            break;
        case 10:
            if (nusound_state[slot] == NU_ST_KEYONWAIT) {
                nusound_state[slot] = NU_ST_PLAYING;
                nusound_started[slot] = 0;
            }
            break;
        case 11: nusound_state[slot] = NU_ST_PLAYWAIT; nusound_started[slot] = 0; break;
        }
        if (before != nusound_state[slot])
            ps2_log("nusound: stream %u state %u -> %u (cmd %u wait_go=%u field=%llu)",
                    slot, before, nusound_state[slot], cmd, nusound_wait_go[slot],
                    (unsigned long long)ps2_kernel_vblank_count());
    }
}
static int nusound_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize, u32 recv, int rsize) {
    (void)ctx;
    nusound_calls++;
    extern int ps2_audio_running(void);
    static u64 audio_field, audio_fraction;
    u64 now = ps2_kernel_vblank_count(), delta = now - audio_field;
    audio_field = now;
    if (!ps2_audio_running() && delta && ps2_nustream_voice_mask()) {
        s16 silent[1024*2], volumes[48][2] = {{0}};
        audio_fraction += delta * 48000u * 1001u;
        u64 frames = audio_fraction / 60000u;
        audio_fraction %= 60000u;
        while (frames) {
            u32 n = frames > 1024 ? 1024 : (u32)frames;
            memset(silent, 0, n*4u);
            ps2_nustream_mix(silent, n, volumes);
            frames -= n;
        }
    }
    if (fno == 1 && send) for (int off = 0; off + 64 <= ssize; off += 64) {
        u32 cmd = ps2_r32(send + off), a[7];
        if (!cmd && off) break;
        nusound_cmds++;
        nusound_first(cmd);
        if (cmd == 9) {
            if (off + 256 > ssize) { ps2_log("nustream: truncated filename packet"); break; }
            for (u32 j = 0; j < 252; j++) nusound_filename[j] = (char)ps2_r8(send + off + 4 + j);
            nusound_filename[252] = 0;
            off += 192;
            continue;
        }
        for (u32 j = 0; j < 7; j++) a[j] = ps2_r32(send + off + 4 + j*4);
        if (cmd) {
            if (cmd != 10 || (a[0] < 8 && nusound_state[a[0]] == NU_ST_KEYONWAIT))
                ps2_nustream_command(cmd, a, nusound_filename);
            nusound_control(cmd, a);
        }
        if (cmd && getenv("PS2_TRACE_NUSOUND"))
            ps2_log("nusound: cmd %u %08X %08X %08X %08X %08X %08X %08X",
                    cmd, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    }
    zero_reply(recv, rsize);
    if (recv) for (u32 i = 0; i < 8 && (int)(i*32+32) <= rsize; i++) {
        u32 position, ended, slot = recv + i*32;
        ps2_nustream_status(i, &position, &ended);
        ps2_w32(slot, nusound_state[i]);
        ps2_w32(slot + 4, position);
        ps2_w32(slot + 12, nusound_started[i] && nusound_state[i] < NU_ST_GOWAIT);
        ps2_w32(slot + 16, ended);
        static u32 last_end[8];
        static u64 last_field[8];
        u64 field = ps2_kernel_vblank_count();
        if ((ended && !last_end[i]) || (nusound_state[i] >= NU_ST_KEYONWAIT && field - last_field[i] >= 300)) {
            ps2_log("nustream: status field=%llu slot=%u state=%u position=%08X eof=%u",
                    (unsigned long long)field, i, nusound_state[i], position, ended);
            last_field[i] = field;
            ps2_nustream_report(i);
        }
        last_end[i] = ended;
    }
    return 0;
}

void ps2_nusound_register(void) {
    ps2_rpc_register(NUSOUND_SID, nusound_rpc, "nusound");
}

int ps2_nusound_packet_selftest(void) {
    const u32 send = 0x10000;
    u8 packet[384] = {0};
    const char *name = "cd:\\BIN\\RADIOEE.PAC";
    u32 cmd = 9;
    memcpy(packet, &cmd, 4); memcpy(packet + 4, name, strlen(name) + 1);
    cmd = 8; memcpy(packet + 256, &cmd, 4);
    cmd = ~0u; memcpy(packet + 264, &cmd, 4);
    ps2_put_mem(send, packet, 320);
    memset(nusound_state, 0, sizeof nusound_state);
    nusound_rpc(NULL, 1, send, 320, 0, 0);
    int fail = strcmp(nusound_filename, name) != 0 || nusound_state[0] != NU_ST_INIT;
    cmd = 14; memcpy(packet + 320, &cmd, 4);
    cmd = 1; memcpy(packet + 328, &cmd, 4);
    ps2_put_mem(send, packet, sizeof packet);
    nusound_rpc(NULL, 1, send, sizeof packet, 0, 0);
    if (!nusound_wait_go[0]) fail++;
    u32 args[7] = {0};
    nusound_control(2, args);
    args[0] = 5;
    nusound_control(14, args);
    nusound_control(7, args);
    if (nusound_state[5] != NU_ST_KEYONWAIT || !nusound_started[5]) fail++;
    nusound_control(10, args);
    if (nusound_state[5] != NU_ST_PLAYING || nusound_started[5]) fail++;
    nusound_control(11, args);
    args[1] = 1;
    nusound_control(14, args);
    nusound_control(7, args);
    if (nusound_state[5] != NU_ST_GOWAIT || nusound_started[5]) fail++;
    nusound_control(10, args);
    if (nusound_state[5] != NU_ST_GOWAIT) fail++;
    args[0] = 1; nusound_control(12, args);
    if (nusound_state[5] != NU_ST_GOWAIT) fail++;
    args[0] = 1u << 5; nusound_control(12, args);
    if (nusound_state[5] != NU_ST_KEYONWAIT || !nusound_started[5]) fail++;
    args[0] = 5; nusound_control(10, args);
    if (nusound_state[5] != NU_ST_PLAYING || nusound_started[5]) fail++;
    nusound_control(2, args);
    ps2_log("nusound start-mode selftest: %s (automatic, held, mask, key-on ACK)", fail ? "FAILED" : "passed");
    ps2_log("nusound packet selftest: %s (256-byte filename, following commands, loop command does not start)", fail ? "FAILED" : "passed");
    return fail;
}

void ps2_nusound_report(void) {
    ps2_log("nusound: %llu calls, %llu commands, %u distinct",
            (unsigned long long)nusound_calls,
            (unsigned long long)nusound_cmds, nusound_nseen);
}


void ps2_dump_archive(const char *when) {
    u32 app = ps2_r32(0x004432ACu);
    u32 ar  = app ? ps2_r32(app + 19540u) : 0;
    int s;
    if (!ar) { ps2_log("archive[%s]: app=%08X, no object", when, app); return; }
    ps2_log("archive[%s]: app=%08X obj=%08X mode=%u tbl{n=%u p=%08X} pacend=%08X",
            when, app, ar, ps2_r8(ar), ps2_r32(ar + 56), ps2_r32(ar + 64),
            ps2_r32(ar + 80));
    for (s = 0; s < 6; s++) {
        u32 c = ar + 84u + 20u * (u32)s;
        u32 n = ps2_r32(c), base = ps2_r32(c + 4), offs = ps2_r32(c + 8);
        u32 e0 = offs ? ps2_r32(offs + 0) : 0;
        u32 e9 = (offs && n > 9) ? ps2_r32(offs + 36) : 0;
        u32 ec = (offs && n > 12) ? ps2_r32(offs + 48) : 0;
        ps2_log("  dir %d: count=%u base=%08X offs=%08X  [0]=%08X [9]=%08X "
                "[12]=%08X  extra=%08X %08X", s, n, base, offs, e0, e9, ec,
                ps2_r32(c + 12), ps2_r32(c + 16));
        if (n && base) {
            u32 which[2] = { e9, ec };
            int k;
            for (k = 0; k < 2; k++) {
                u32 p = which[k] + base, hdr, w, h, fmt, dat, i, nz = 0, total;
                char line[160];
                int fill;
                if (!which[k]) continue;
                hdr = p + 16;
                fmt = ps2_r8(hdr + 3);
                w   = ps2_r16(hdr + 12);
                h   = ps2_r16(hdr + 14);
                dat = p + 32;
                ps2_log("       res%d @%08X magic=%02X%02X%02X%02X flags=%08X "
                        "fmt=%02X %ux%u", k ? 12 : 9, p,
                        ps2_r8(p), ps2_r8(p+1), ps2_r8(p+2), ps2_r8(p+3),
                        ps2_r32(p + 4), fmt, w, h);
                total = w * h;
                if (fmt == 0x14 || fmt == 0x24 || fmt == 0x2C) total = (total + 1) / 2;
                else if (fmt == 0x13 || fmt == 0x1B) total = total;
                else if (fmt == 0) total *= 4; else if (fmt == 1) total *= 3;
                else total *= 2;
                if (total > 0x100000u) total = 0x100000u;
                for (i = 0; i < total; i++) if (ps2_r8(dat + i)) nz++;
                fill = 0;
                for (i = 0; i < 32 && fill < 150; i++)
                    fill += snprintf(line + fill, sizeof line - (size_t)fill,
                                     "%02X", ps2_r8(dat + i));
                ps2_log("         data %u bytes, %u nonzero: %s", total, nz, line);
            }
        }
    }
}

static int stop_major = -1, stop_minor = -1, stop_frames;
static int stop_seen;

void ps2_stop_scene(int major, int minor, int frames) {
    stop_major = major;
    stop_minor = minor;
    stop_frames = frames > 0 ? frames : 90;
    stop_seen = 0;
    if (!getenv("PS2_DIAG_AFTER")) ps2_diag_armed = 0;
}

void ps2_stop_scene_check(unsigned major, unsigned minor) {
    if (stop_major < 0) return;
    if ((int)major != stop_major) return;
    if (stop_minor >= 0 && (int)minor != stop_minor) return;
    if (++stop_seen == 1 && !ps2_diag_armed) {
        ps2_diag_armed = 1;
        ps2_log("diag: censuses and texture dumps armed at scene %u.%u",
                major, minor);
    }
    if (stop_seen < stop_frames) return;
    ps2_log("scene: reached %d.%d for %d frames -- stopping",
            stop_major, stop_minor, stop_frames);
    ps2_finish("stop scene");
}

static void scene_table_scan(u32 obj) {
    u32 tab = ps2_r32(obj);
    u32 lo, hi, major;
    unsigned handlers = 0, missing = 0, levels = 0;
    ps2_text_bounds(&lo, &hi);
    if (!tab || tab < 0x00080000u || tab >= PS2_RAM_SIZE) {
        ps2_log("scene: table pointer %08X is not plausible; not scanned", tab);
        return;
    }
    for (major = 0; major < 256u; major++) {
        u32 lvl = ps2_r32(tab + 4u * major);
        u32 minor;
        if (!lvl || lvl < 0x00080000u || lvl + 1024u >= PS2_RAM_SIZE) continue;
        if (!ps2_fn_known(ps2_r32(lvl))) {
            u32 f0 = ps2_r32(lvl);
            if (f0 < lo || f0 >= hi || (f0 & 3u)) continue;
        }
        levels++;
        for (minor = 0; minor < 256u; minor++) {
            u32 fn = ps2_r32(lvl + 4u * minor);
            if (fn < lo || fn >= hi || (fn & 3u)) break;
            handlers++;
            if (ps2_fn_known(fn)) continue;
            missing++;
            ps2_log("scene: state %u.%u -> %08X  NOT RECOMPILED",
                    major, minor, fn);
        }
    }
    if (missing)
        ps2_log("scene: %u of %u handlers across %u states are missing; "
                "those screens will not work.  Seed them in "
                "config/manual_symbols.json and recompile.",
                missing, handlers, levels);
    else
        ps2_log("scene: all %u handlers across %u states are recompiled",
                handlers, levels);
}

static struct { u32 obj, key; u64 calls; } speed_scene[16];
void ps2_scene_speed_report(double dt) {
    for (unsigned i=0;i<16;i++) if (speed_scene[i].obj) {
        if (dt > 0) ps2_log("speed: scene obj=%08X state=%u.%u dispatch=%.2f/s",
            speed_scene[i].obj, speed_scene[i].key >> 8, speed_scene[i].key & 255,
            speed_scene[i].calls/dt);
        speed_scene[i].calls=0;
    }
}
void hook_scene(ps2_ctx *ctx) {
    static struct { u32 obj, key; } last[16];
    static u32 scanned[8];
    static unsigned scanned_n;
    u32 obj = ps2_arg(ctx, 0);
    {
        unsigned i;
        for (i = 0; i < scanned_n; i++) if (scanned[i] == obj) break;
        if (i == scanned_n && scanned_n < 8) {
            scanned[scanned_n++] = obj;
            ps2_log("scene: scanning machine %u (object %08X, table %08X)",
                    scanned_n - 1, obj, ps2_r32(obj));
            scene_table_scan(obj);
        }
    }
    {
        static int tl = -1;
        static u32 last_sub = 0xFFFFFFFFu, last_st = 0xFFFFFFFFu;
        static u32 last_mode2[16], last_bst2[16], last_hdr2[16];
        static int last_fh[16];
        static int last_sw2[16];
        static int init_hist;
        if (!init_hist) { init_hist = 1;
            for (int k = 0; k < 16; k++) { last_mode2[k] = last_bst2[k] =
                last_hdr2[k] = 0xFFFFFFFFu; last_fh[k] = 0x7FFFFFFF;
                last_sw2[k] = 0x7FFFFFFF; } }
#define last_mode last_mode2
#define last_bst  last_bst2
#define last_hdr  last_hdr2
        if (tl < 0) { const char *e = getenv("PS2_TRACE_LOAD");
                      tl = (e && *e && *e != '0'); }
        if (tl) {
            u32 base = ps2_r32(0x00448270u - 20420u);
            u32 o    = base ? ps2_r32(base + 19596u) : 0u;
            u32 sub  = o ? ps2_r32(o + 44u) : 0xFFFFFFFEu;
            u32 io   = base ? ps2_r32(base + 19592u) : 0u;
            u32 st   = io ? ps2_r32(io + 132u) : 0xFFFFFFFEu;
            {   static u32 last_t[4], last_c[4];
                u32 rq = ps2_r32(0x00448270u - 17008u), q;
                for (q = 0; rq && q < 4u; q++) {
                    u32 t = ps2_r32(rq + 20u + 4u * q);
                    u32 c = ps2_r32(rq + 32u + 4u * q);
                    if (t == last_t[q] && c == last_c[q]) continue;
                    last_t[q] = t; last_c[q] = c;
                    ps2_log("disc: queue %u  ticket=%u completed=%u%s", q, t, c,
                            t != c ? "   <-- read outstanding" : "");
                }
            }
            u32 flg  = io ? ps2_r32(io + 120u) : 0u;
            u32 sobj = base ? ps2_r32(base + ((flg & 0x20u) ? 19536u : 19532u))
                            : 0u;
            u32 stab = sobj ? ps2_r32(sobj + 132u) : 0u;
            int pch  = -1;
            if (stab) {
                u32 i = ps2_r8(sobj + 141u);
                u32 j = ps2_r8(stab + 20u);
                pch = (int)(signed char)ps2_r8(stab + i + 4u * j);
            }
            {
                u32 arr = ps2_r32(0x00448270u - 10164u);
                {
                    static u32 said;
                    if (arr && !said) { said = 1;
                        ps2_log("snd: channel array at %08X", arr); }
                }
                if (arr) {
                    static u32 last_rd = 0xFFFFFFFFu, last_wr = 0xFFFFFFFFu;
                    u32 rd = ps2_r32(arr + 102416u);
                    u32 wr = ps2_r32(arr + 102420u);
                    if (rd != last_rd || wr != last_wr) {
                        last_rd = rd; last_wr = wr;
                        ps2_log("snd: cmd ring read=%u write=%u%s", rd, wr,
                                rd == wr ? "  (empty)" : "  <-- pending");
                        for (u32 q = 0; q < 16u; q++) {
                            u32 e = arr + 140u * q + 102428u;
                            u32 op = ps2_r8(e);
                            char nm[64];
                            if (!op) continue;
                            for (u32 c = 0; c < sizeof(nm) - 1u; c++) {
                                nm[c] = (char)ps2_r8(e + 24u + c);
                                if (!nm[c]) break;
                                if (nm[c] < 32 || nm[c] > 126) { nm[c] = 0; break; }
                                nm[c + 1u] = 0;
                            }
                            nm[sizeof(nm) - 1u] = 0;
                            ps2_log("     cmd[%u] op=%u('%c') ch=%u '%s'",
                                    q, op, (op >= 32 && op < 127) ? (char)op : '?',
                                    ps2_r32(e + 4u), nm);
                        }
                    }
                }
                for (u32 h = 0; arr && h < 16u; h++) {
                    u32 chan = arr + h * 10240u;
                    u32 mode = ps2_r32(chan + 16u);
                    u32 blk  = ps2_r32(chan + 4360u);
                    u32 bst  = blk ? ps2_r32(blk) : 0xFFFFFFFFu;
                    u32 pos  = blk ? ps2_r32(blk + 4u) : 0u;
                    u32 live = ps2_r8(chan + 8u);
                    u32 hdr  = ps2_r32(chan + 4096u);
                    u32 f100 = ps2_r8(chan + 4100u);
                    u32 f428 = ps2_r8(chan + 4428u);
                    u32 bufsz = ps2_r32(chan + 40u);
                    u32 h12 = hdr ? ps2_r32(hdr + 12u) : 0u;
                    u32 h16 = hdr ? ps2_r32(hdr + 16u) : 0u;
                    int fh = (int)ps2_r32(chan + 64u);
                    u32 f101 = ps2_r8(chan + 4101u);
                    u32 f103 = ps2_r8(chan + 4103u);
                    u32 prev = ps2_r32(chan + 32u);
                    int sw;
                    if (!live)          sw = 99;
                    else if (mode == 1) sw = 1;
                    else if (mode == 2) sw = (bst == 4u) ? 2 : 1;
                    else if (mode == 3) sw = (bst == 4u) ? 2
                                           : (f101 ? 4 : (int)mode);
                    else if (mode == 0) sw = (bst != 1u) ? (int)prev : 0;
                    else                sw = 99;
                    if (mode && f103)   sw = 5;
                    if (mode != last_mode[h] || bst != last_bst[h]
                        || hdr != last_hdr[h] || fh != last_fh[h]
                        || sw != last_sw2[h]) {
                        last_fh[h] = fh;
                        last_mode[h] = mode; last_bst[h] = bst;
                        last_hdr[h] = hdr; last_sw2[h] = sw;
                        ps2_log("snd: ch%u active=%u mode=%u blk=%08X "
                                "state=%d pos=%u | hdr=%08X +4100=%u "
                                "+4428=%u buf=%u hdr12=%u hdr16=%u",
                                h, live, mode, blk, (int)bst, pos,
                                hdr, f100, f428, bufsz, h12, h16);
                        ps2_log("     ch%u handle=%d | +4101=%u +4103=%u "
                                "+32=%u -> status %d%s",
                                h, fh, f101, f103, prev, sw,
                                sw == 2 ? "  <-- loader state 3 would advance"
                                        : "");
                    }
                }
            }
            {
            static u32 last_flg = 0xFFFFFFFFu;
            static int last_pch = 0x7FFFFFFF;
            if (sub != last_sub || st != last_st
                || flg != last_flg || pch != last_pch) {
                last_sub = sub;
                last_st = st;
                last_flg = flg;
                last_pch = pch;
                ps2_log("load: sub-state %u  loader-state %u  at field %llu "
                        "(base=%08X obj=%08X io=%08X) flags=%08X "
                        "sndobj=%08X polls ch%d", sub, st,
                        (unsigned long long)ps2_kernel_vblank_count(),
                        base, o, io, flg, sobj, pch);
            }
            }
        }
    }
    u32 major = ps2_r8(obj + 8), minor = ps2_r8(obj + 9);
    u32 key = (major << 8) | minor;
    unsigned scene_slot;
    for (scene_slot=0;scene_slot<16;scene_slot++)
        if (!last[scene_slot].obj || last[scene_slot].obj == obj) break;
    if (scene_slot < 16) {
        extern int ps2_speed_enabled(void);
        if (ps2_speed_enabled()) {
            speed_scene[scene_slot].obj=obj; speed_scene[scene_slot].key=key;
            speed_scene[scene_slot].calls++;
        }
        if (last[scene_slot].obj == obj && last[scene_slot].key == key) return;
        last[scene_slot].obj=obj; last[scene_slot].key=key;
    }
    {
        u32 tab = ps2_r32(obj);
        u32 lvl = ps2_r32(tab + 4u * major);
        u32 fn = lvl ? ps2_r32(lvl + 4u * minor) : 0;
        ps2_log("scene: field %llu  state %u.%u -> %08X",
                (unsigned long long)ps2_kernel_vblank_count(), major, minor, fn);
    }
    ps2_stop_scene_check(major, minor);
    if (ps2_verbose) {
        char w[16];
        snprintf(w, sizeof w, "%u.%u", major, minor);
        ps2_dump_archive(w);
    }
}

void hook_scene_next(ps2_ctx *ctx) {
    u32 obj = ps2_arg(ctx, 0);
    ps2_log("scene: transition from %u.%u", ps2_r8(obj + 8), ps2_r8(obj + 9));
}

#define NUSNDSTR_SID 0x81000201u

#define NUSNDSTR_SEQ 0x0047EC9Cu

static u32 nusndstr_seq;
static u64 nusndstr_transfers, nusndstr_bytes;

#define NUSNDSTR_NCMD 18u

static const u8 nusndstr_size[NUSNDSTR_NCMD] = {
    0,
    12,
    12,
    20,
    0,
    0,
    12,
    8,
    0,
    0,
    12,
    12,
    32,
    32,
    32,
    0,
    16,
    16,
};

static const char *const nusndstr_cmd_name[NUSNDSTR_NCMD] = {
    "?", "fx-send-on", "fx-send-off", "transfer", "?", "?", "autodma-vol",
    "output-mode", "?", "?", "key-on", "key-off", "tone-attr",
    "stream-vol", "stream-pitch", "?", "effect-mode", "effect-attr"
};

static u64 nusndstr_census[NUSNDSTR_NCMD];
static u64 nusndstr_lists, nusndstr_desync;
static u32 nusndstr_seq_seen;
static struct { u32 command, token; } nusndstr_ack[48];
static u16 nusndstr_output_mode;
void ps2_spu2_command_lock(void);
void ps2_spu2_command_unlock(void);
u8 ps2_spu2_voice_status(u32);
void ps2_spu2_voice_pitch(u32,u32);

static u32 nusndstr_ssa_last[48];
static u64 nusndstr_ssa_good, nusndstr_ssa_poor, nusndstr_ssa_zero;

static void nusndstr_check_ssa(u32 ch, u32 ssa) {
    unsigned ok = 0, k;
    if (ch >= 48u || ssa == nusndstr_ssa_last[ch]) return;
    nusndstr_ssa_last[ch] = ssa;
    if (!ssa) { nusndstr_ssa_zero++; return; }
    if (!ps2_spu2_ram || ssa > PS2_SPU2_RAM_SIZE - 32u * 16u) {
        nusndstr_ssa_poor++;
        if (nusndstr_ssa_poor <= 8)
            ps2_log("nusndstr: voice %u invalid sample address %08X (diagnostic skipped)", ch, ssa);
        return;
    }
    for (k = 0; k < 32u; k++) {
        const u8 *b = ps2_spu2_ram + ssa + k * 16u;
        if ((b[0] >> 4) <= 4u && b[1] <= 7u) ok++;
    }
    if (ok >= 31u) nusndstr_ssa_good++;
    else {
        nusndstr_ssa_poor++;
        if (nusndstr_ssa_poor <= 8)
            ps2_log("nusndstr: voice %u pointed at %06X, only %u/32 blocks "
                    "look like ADPCM", ch, ssa, ok);
    }
}

static int radio_ptr(u32 p, u32 size) {
    p &= 0x1fffffffu;
    return p && size <= PS2_RAM_SIZE && p <= PS2_RAM_SIZE - size;
}
static int radio_trace(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("PS2_SPEED_LOG") || getenv("PS2_TRACE_RADIO");
    return enabled;
}
void hook_radio_request(ps2_ctx *ctx) {
    if (!radio_trace()) return;
    u32 q = ps2_arg(ctx, 0), list = ps2_arg(ctx, 1), cond = ps2_arg(ctx, 4);
    if (!radio_ptr(q, 5956) || !radio_ptr(list, 4) || !radio_ptr(cond, 2)) return;
    u32 attr = ps2_r32(list);
    if (!radio_ptr(attr, 32)) return;
    ps2_log("radio: request field=%llu queue=%08X table=%u script=%d speaker=%u priority=%d flags=%02X min_delay_ms=%d expiry_ms=%d count=%u pending=%u",
            (unsigned long long)ps2_kernel_vblank_count(), q, ps2_r16(attr),
            (s16)ps2_r16(attr+2), ps2_r8(attr+17), (s8)ps2_r8(cond), ps2_r8(cond+1),
            (s32)ps2_r32(attr+28), (s32)ps2_r32(attr+24), ps2_r32(q+4), ps2_r16(q+5936));
}
void hook_radio_select(ps2_ctx *ctx) {
    if (!radio_trace()) return;
    u32 node = ps2_arg(ctx, 1);
    if (!radio_ptr(node, 180)) return;
    u32 attr = ps2_r32(node+8);
    if (!radio_ptr(attr, 32)) return;
    ps2_log("radio: selected field=%llu state=%08X channel=%u table=%u script=%d speaker=%u remaining_delay_ms=%d remaining_expiry_ms=%d",
            (unsigned long long)ps2_kernel_vblank_count(), ps2_arg(ctx, 0), ps2_arg(ctx, 3),
            ps2_r16(attr), (s16)ps2_r16(attr+2), ps2_r8(attr+17),
            (s32)ps2_r32(node+164), (s32)ps2_r32(node+160));
}
void hook_radio_queue(ps2_ctx *ctx) {
    if (!radio_trace()) return;
    u32 m = ps2_arg(ctx, 0);
    if (!radio_ptr(m, 16)) return;
    u32 q = ps2_r32(m+4), state = ps2_r32(m+8);
    if (!radio_ptr(q, 5956) || !radio_ptr(state, 512)) return;
    u32 count = ps2_r32(q+4), head = ps2_r32(q+5948), attr = 0;
    if (count && radio_ptr(head, 180)) attr = ps2_r32(head+8);
    u32 major = ps2_r32(state+304), minor = ps2_r32(state+308);
    static u32 last[6]; static u64 last_field;
    u32 current[6] = {m, count, attr, major, minor, ps2_r32(m)};
    u64 field = ps2_kernel_vblank_count();
    if (!memcmp(last, current, sizeof last) && field-last_field < 300) return;
    memcpy(last, current, sizeof last); last_field = field;
    if (!count) return;
    ps2_log("radio: queue field=%llu manager=%08X queue=%08X count=%u disabled=%u state=%u.%u channel=%u head_attr=%08X delay_ms=%d expiry_ms=%d",
            (unsigned long long)field, m, q, count, ps2_r32(m), major, minor, ps2_r32(state+4), attr,
            radio_ptr(head,180) ? (s32)ps2_r32(head+164) : -1,
            radio_ptr(head,180) ? (s32)ps2_r32(head+160) : -1);
}

int ps2_nusndstr_address_selftest(void) {
    const u32 bad[] = {0xffffffffu, 0xfffffff0u, 0xfffffe00u,
                      PS2_SPU2_RAM_SIZE, PS2_SPU2_RAM_SIZE - 511u};
    u64 before = nusndstr_ssa_poor;
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; i++)
        nusndstr_check_ssa(47, bad[i]);
    int fail = nusndstr_ssa_poor - before != sizeof bad / sizeof bad[0];
    before = nusndstr_ssa_poor;
    memset(ps2_spu2_ram + PS2_SPU2_RAM_SIZE - 512u, 0, 512u);
    nusndstr_check_ssa(47, PS2_SPU2_RAM_SIZE - 512u);
    if (nusndstr_ssa_poor != before) fail++;
    ps2_log("nusndstr address selftest: %s (wraparound and end-of-RAM boundaries)", fail ? "FAILED" : "passed");
    return fail;
}

static u64 nusndstr_walk(u32 send, int ssize) {
    u32 count = ps2_r32(send);
    u32 claimed = count;
    u32 off = 4, n = 0;
    u64 total = 0;
    const char *bad = NULL;

    nusndstr_lists++;
    if (count > 0x1000u / 8u) {
        bad = "record count is impossible";
        count = 0;
    }
    while (n < count) {
        u32 cmd, sz;
        if (off + 4u > (u32)ssize) { bad = "ran off the end of the buffer"; break; }
        cmd = ps2_r32(send + off);
        sz  = (cmd < NUSNDSTR_NCMD) ? nusndstr_size[cmd] : 0u;
        if (!sz)                        { bad = "unknown command word";     break; }
        if (off + sz > (u32)ssize)      { bad = "record overruns the list";  break; }
        nusndstr_census[cmd]++;
        if(cmd>=12u&&cmd<=14u) {
            u32 voice=ps2_r8(send+off+8);
            if(voice<48) {
                nusndstr_ack[voice].command=cmd;
                nusndstr_ack[voice].token=ps2_r32(send+off+4);
            }
        }
        switch (cmd) {
        case 3u: {
            u32 src = ps2_r32(send + off + 4u);
            u32 dst = ps2_r32(send + off + 8u);
            u32 len = ps2_r32(send + off + 12u);
            u32 seq = ps2_r32(send + off + 16u);
            if (seq != nusndstr_seq_seen + 1u && nusndstr_seq_seen)
                ps2_log("nusndstr: transfer sequence jumped %u -> %u; the "
                        "record sizes may be wrong",
                        nusndstr_seq_seen, seq);
            nusndstr_seq_seen = seq;
            ps2_spu2_write(dst, src, len);
            total += len;
            break;
        }
        case 12u: {
            u32 ch    = ps2_r32(send + off +  8u) & 0xFFu;
            u32 voll  = ps2_r32(send + off + 12u) & 0xFFFFu;
            u32 volr  = ps2_r32(send + off + 12u) >> 16;
            u32 adsr1 = ps2_r32(send + off + 16u) & 0xFFFFu;
            u32 adsr2 = ps2_r32(send + off + 16u) >> 16;
            u32 pitch = ps2_r32(send + off + 20u) & 0xFFFFu;
            u32 ssa   = ps2_r32(send + off + 24u);
            u32 loop  = ps2_r32(send + off + 28u) & 0xFFu;
            if (ch < 48u) {
                nusndstr_check_ssa(ch, ssa);
                ps2_spu2_set_voice(ch, ssa, pitch, adsr1, adsr2, voll, volr,
                                   (int)loop);
            }
            break;
        }
        case 13u:
            ps2_spu2_stream_volume(ps2_r8(send + off + 8u),
                                  ps2_r16(send + off + 12u), ps2_r16(send + off + 14u));
            break;
        case 7u:
            nusndstr_output_mode = ps2_r16(send + off + 4u);
            break;
        case 14u:
            ps2_spu2_voice_pitch(ps2_r8(send+off+8u),ps2_r16(send+off+20u));
            break;
        case 10u:
            ps2_spu2_key_on_mask(ps2_r32(send + off + 4u),
                                 ps2_r32(send + off + 8u));
            break;
        case 11u:
            ps2_spu2_key_off_mask(ps2_r32(send + off + 4u),
                                  ps2_r32(send + off + 8u));
            break;
        case 1u:
        case 2u:
            break;
        default: break;
        }
        off += sz;
        n++;
    }
    if (bad) {
        static int said;
        nusndstr_desync++;
        if (!said || getenv("PS2_NUSNDSTR_STRICT")) {
            said = 1;
            ps2_log("nusndstr: list desync -- %s at byte %u of %d, after %u of "
                    "%u records", bad, off, ssize, n, claimed);
        }
        if (getenv("PS2_NUSNDSTR_STRICT"))
            ps2_fatal("nusndstr: refusing to continue past a desynced command "
                      "list (PS2_NUSNDSTR_STRICT)");
    }
    if (getenv("PS2_TRACE_NUSNDSTR"))
        ps2_log("nusndstr: list of %u records, %u bytes%s",
                count, off, bad ? " (DESYNCED)" : "");
    return total;
}

static int nusndstr_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                        u32 recv, int rsize) {
    (void)ctx;
    ps2_spu2_command_lock();
    memset(nusndstr_ack,0,sizeof nusndstr_ack);
    if (fno == 0) {
        nusndstr_seq = ps2_r32(NUSNDSTR_SEQ);
        nusndstr_seq_seen = 0;
    }
    if (fno == 1) {
        u32 issued = ps2_r32(NUSNDSTR_SEQ);
        if (send && ssize >= 4)
            nusndstr_bytes += nusndstr_walk(send, ssize);
        if (issued >= nusndstr_seq && issued - nusndstr_seq < 0x100000u) {
            if (issued != nusndstr_seq && nusndstr_transfers < 4)
                ps2_log("nusndstr: SPU transfer sequence %u -> %u complete",
                        nusndstr_seq, issued);
            nusndstr_transfers += issued - nusndstr_seq;
            nusndstr_seq = issued;
        } else {
            static int said;
            if (!said) {
                said = 1;
                ps2_log("nusndstr: issued counter at %08X reads %08X, which "
                        "cannot follow %u -- ignoring it; sound banks may "
                        "never report complete", NUSNDSTR_SEQ, issued,
                        nusndstr_seq);
            }
        }
    }
    zero_reply(recv, rsize);
    if (recv && rsize >= 16 && fno != 0) {
        ps2_w32(recv + 0, 1);
        ps2_w32(recv + 4, 3);
        ps2_w32(recv + 8, nusndstr_seq);
        ps2_w32(recv + 12, 0);
        u32 off=16,count=1;
        if(rsize>=72) {
            for(u32 i=0;i<48;i++) if(nusndstr_ack[i].command&&off+12+56<=(u32)rsize) {
                ps2_w32(recv+off,nusndstr_ack[i].command);
                ps2_w32(recv+off+4,nusndstr_ack[i].token);
                ps2_w8(recv+off+8,(u8)i); ps2_w8(recv+off+9,1);
                off+=12; count++;
            }
            ps2_w32(recv+off,8);
            ps2_w16(recv+off+4,nusndstr_output_mode);
            for(u32 i=0;i<48;i++) ps2_w8(recv+off+6+i,ps2_spu2_voice_status(i));
            ps2_w32(recv,count+1);
        }
    }
    ps2_spu2_command_unlock();
    return 0;
}

void ps2_nusndstr_register(void) {
    ps2_rpc_register(NUSNDSTR_SID, nusndstr_rpc, "nusndstr");
}

int ps2_nusndstr_fx_send_selftest(void) {
    const u32 send = 0x10000;
    u32 fx[7]   = {2,  1u, 1u << 2, 0,  2u, 1u << 3, 0};
    u32 keys[7] = {2, 10u, 1u << 2, 0, 11u, 1u << 3, 0};
    int fail = 0;
    ps2_spu2_key_on_mask(1u << 3, 0);
    ps2_put_mem(send, (u8 *)fx, sizeof fx);
    nusndstr_walk(send, (int)sizeof fx);
    fail += ps2_spu2_voice_status(2) != 0;
    fail += ps2_spu2_voice_status(3) != 3;
    ps2_put_mem(send, (u8 *)keys, sizeof keys);
    nusndstr_walk(send, (int)sizeof keys);
    fail += ps2_spu2_voice_status(2) != 3;
    fail += ps2_spu2_voice_status(3) != 0;
    ps2_spu2_key_off_mask(1u << 2, 0);
    ps2_log("nusndstr fx-send selftest: %s (commands 1/2 route, 10/11 key)",
            fail ? "FAILED" : "passed");
    return fail;
}

void ps2_nusndstr_report(void) {
    u32 i;
    ps2_log("nusndstr: %llu SPU transfers, %llu bytes, sequence %u",
            (unsigned long long)nusndstr_transfers,
            (unsigned long long)nusndstr_bytes, nusndstr_seq);
    ps2_log("nusndstr: %llu command lists walked, %llu desynced",
            (unsigned long long)nusndstr_lists,
            (unsigned long long)nusndstr_desync);
    for (i = 0; i < NUSNDSTR_NCMD; i++)
        if (nusndstr_census[i])
            ps2_log("nusndstr:   cmd %-2u %-13s %llu", i, nusndstr_cmd_name[i],
                    (unsigned long long)nusndstr_census[i]);
    if (nusndstr_ssa_good || nusndstr_ssa_poor || nusndstr_ssa_zero)
        ps2_log("nusndstr: distinct waveform addresses -- %llu valid ADPCM, "
                "%llu not, %llu null",
                (unsigned long long)nusndstr_ssa_good,
                (unsigned long long)nusndstr_ssa_poor,
                (unsigned long long)nusndstr_ssa_zero);
}
