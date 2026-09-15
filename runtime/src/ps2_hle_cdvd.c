#include "ps2_runtime.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <string.h>

#define SCECdComplete       0x02
#define SCECdNotReady       0x06
#define SCECdStatStop       0x00
#define SCECdStatSpin       0x02
#define SCECdStatPause      0x0a
#define SCECdErNO           0x00
#define SCECdErREAD         0x30
#define SCECdErNODISC       0x12
#define SCECdPS2DVD         0x14
#define SCECdCD             1
#define SCECdDVD            2
#define SCECdFuncRead       1
#define SCECdFuncGetToc     3
#define SCECdFuncSeek       4

#define CDVD_SECTOR 2048

static u32 cd_callback;
static u32 cd_poff_callback, cd_poff_arg;
static int cd_error = SCECdErNO;
static int cd_media = SCECdDVD;
static u32 cd_read_pos;
static u64 cd_reads, cd_sectors_read, cd_searches, cd_search_misses;
static int cd_inited;

static void raise_callback(ps2_ctx *ctx, int reason) {
    if (!cd_callback) return;
    {
        ps2_ctx sub = *ctx;
        sub.r[4].sd[0] = reason;
        ps2_dispatch(&sub, cd_callback);
    }
}

void ps2_cdvd_init(void) {
    cd_callback = cd_poff_callback = cd_poff_arg = 0;
    cd_error = SCECdErNO;
    cd_media = SCECdDVD;
    cd_read_pos = 0;
    cd_reads = cd_sectors_read = cd_searches = cd_search_misses = 0;
    cd_inited = 0;
}

int ps2_cdvd_sync_pending(void) { return 0; }

void hle_sceCdInit(ps2_ctx *ctx) {
    if (!cd_inited) {
        cd_inited = 1;
        ps2_log("cdvd: init mode=%u, %u files on disc",
                ps2_arg(ctx, 0), ps2_iso_file_count());
    }
    HRET(1);
}

void hle_sceCdInitEeCB(ps2_ctx *ctx) { HRET(1); }

void hle_sceCdCallback(ps2_ctx *ctx) {
    u32 prev = cd_callback;
    cd_callback = ps2_arg(ctx, 0);
    HRET(prev);
}

void hle_sceCdPOffCallback(ps2_ctx *ctx) {
    u32 prev = cd_poff_callback;
    cd_poff_callback = ps2_arg(ctx, 0);
    cd_poff_arg = ps2_arg(ctx, 1);
    HRET(prev);
}

static void fill_cdlfile(u32 fp, const ps2_disc_file *f) {
    const char *base = f->name, *p;
    char nm[16];
    u32 i;
    for (p = f->name; *p; p++)
        if (*p == '/') base = p + 1;
    memset(nm, 0, sizeof(nm));
    for (i = 0; i < 15 && base[i]; i++) nm[i] = base[i];
    ps2_w32(fp + 0, f->lsn);
    ps2_w32(fp + 4, f->size);
    for (i = 0; i < 16; i++) ps2_w8(fp + 8 + i, (u8)nm[i]);
    for (i = 0; i < 8; i++) ps2_w8(fp + 24 + i, f->date[i]);
    ps2_w32(fp + 32, f->flag);
}

static void search_file(ps2_ctx *ctx, u32 fp, u32 name_addr) {
    char name[256];
    const ps2_disc_file *f;
    ps2_get_str(name_addr, name, sizeof(name));
    cd_searches++;
    f = ps2_iso_find(name);
    if (ps2_verbose)
        ps2_log("cdvd: search '%s' -> %s", name,
                f ? "found" : "MISSING");
    if (!f) {
        cd_search_misses++;
        ps2_log("cdvd: '%s' not on disc", name);
        HRET(0);
        return;
    }
    fill_cdlfile(fp, f);
    HRET(1);
}

void hle_sceCdSearchFile(ps2_ctx *ctx) {
    search_file(ctx, ps2_arg(ctx, 0), ps2_arg(ctx, 1));
}

void hle_sceCdLayerSearchFile(ps2_ctx *ctx) {
    if (ps2_arg(ctx, 2) != 0) { HRET(0); return; }
    search_file(ctx, ps2_arg(ctx, 0), ps2_arg(ctx, 1));
}

void hle_sceCdRead(ps2_ctx *ctx) {
    u32 lbn = ps2_arg(ctx, 0);
    u32 sectors = ps2_arg(ctx, 1);
    u32 buf = ps2_arg(ctx, 2);
    int rc;
    if (!ps2_iso_ready()) {
        cd_error = SCECdErNODISC;
        HRET(0);
        return;
    }
    rc = ps2_iso_read(lbn, sectors, buf);
    if (rc < 0) {
        cd_error = SCECdErREAD;
        ps2_log("cdvd: read failed lbn=%u sectors=%u", lbn, sectors);
        HRET(0);
        return;
    }
    if (ps2_verbose || PS2_ENV("PS2_TRACE_MPEG"))
        ps2_log("cdvd: read lbn=%u sectors=%u (%u bytes) -> %08X  "
                "[%08X %08X %08X %08X]",
                lbn, sectors, sectors * 2048u, buf, ps2_r32(buf),
                ps2_r32(buf + 4), ps2_r32(buf + 8), ps2_r32(buf + 12));
    cd_error = SCECdErNO;
    cd_reads++;
    cd_sectors_read += sectors;
    cd_read_pos = lbn + sectors;
    HRET(1);
    raise_callback(ctx, SCECdFuncRead);
}

void hle_sceCdReadIOPm(ps2_ctx *ctx) { hle_sceCdRead(ctx); }

void hle_sceCdSync(ps2_ctx *ctx)  { HRET(0); }
void hle_sceCdSyncS(ps2_ctx *ctx) { HRET(0); }

void hle_sceCdGetError(ps2_ctx *ctx) { HRET(cd_error); }

void hle_sceCdStatus(ps2_ctx *ctx) { HRET(SCECdStatPause); }

void hle_sceCdDiskReady(ps2_ctx *ctx) {
    HRET(ps2_iso_ready() ? SCECdComplete : SCECdNotReady);
}

void hle_sceCdGetDiskType(ps2_ctx *ctx) { HRET(SCECdPS2DVD); }

void hle_sceCdMmode(ps2_ctx *ctx) { cd_media = (int)ps2_arg(ctx, 0); HRET(1); }

void hle_sceCdSeek(ps2_ctx *ctx) {
    cd_read_pos = ps2_arg(ctx, 0);
    HRET(1);
    raise_callback(ctx, SCECdFuncSeek);
}

void hle_sceCdGetReadPos(ps2_ctx *ctx) { HRET(cd_read_pos); }

void hle_sceCdBreak(ps2_ctx *ctx) { HRET(1); }

void hle_sceCdGetToc(ps2_ctx *ctx) {
    u32 toc = ps2_arg(ctx, 0);
    for (u32 i = 0; i < 1024; i++) ps2_w8(toc + i, 0);
    ps2_w8(toc + 0, 0x01);
    ps2_w8(toc + 1, 0x01);
    ps2_w8(toc + 2, 0x01);
    HRET(1);
}

void hle_sceCdStandby(ps2_ctx *ctx) { HRET(1); }
void hle_sceCdStop(ps2_ctx *ctx)    { HRET(1); }
void hle_sceCdPause(ps2_ctx *ctx)   { HRET(1); }
void hle_sceCdTrayReq(ps2_ctx *ctx) {
    u32 chk = ps2_arg(ctx, 1);
    if (chk) ps2_w32(chk, 0);
    HRET(1);
}
void hle_sceCdChangeThreadPriority(ps2_ctx *ctx) { HRET(1); }
void hle_sceCdSetEEReadMode(ps2_ctx *ctx) { HRET(ps2_arg(ctx, 0)); }
void hle_sceCdPowerOff(ps2_ctx *ctx) { HRET(1); }

void hle_sceCdReadClock(ps2_ctx *ctx) {
    u32 p = ps2_arg(ctx, 0);
    ps2_w8(p + 0, 0x00);
    ps2_w8(p + 1, 0x00);
    ps2_w8(p + 2, 0x00);
    ps2_w8(p + 3, 0x12);
    ps2_w8(p + 4, 0x00);
    ps2_w8(p + 5, 0x01);
    ps2_w8(p + 6, 0x01);
    ps2_w8(p + 7, 0x05);
    HRET(1);
}

static int cdvd_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                    u32 recv, int rsize) {
    (void)send; (void)ssize;
    if (recv && rsize > 0)
        for (int i = 0; i < rsize; i += 4) ps2_w32(recv + (u32)i, 0);
    if (recv && rsize >= 4) ps2_w32(recv, 1);
    ps2_log("cdvd: RPC fno=%u served generically (%d/%d bytes)",
            fno, ssize, rsize);
    return 0;
}

static int cdvd_ready_rpc(ps2_ctx *ctx, u32 fno, u32 send, int ssize,
                          u32 recv, int rsize) {
    (void)ctx; (void)fno; (void)send; (void)ssize;
    if (recv && rsize > 0) {
        int i;
        for (i = 0; i < rsize; i += 4) ps2_w32(recv + (u32)i, 0);
    }
    if (recv && rsize >= 4)
        ps2_w32(recv, ps2_iso_ready() ? SCECdComplete : SCECdNotReady);
    return 0;
}

void ps2_cdvd_rpc_register(void) {
    ps2_rpc_register(0x80000592u, cdvd_rpc, "cdvd-scmd");
    ps2_rpc_register(0x80000593u, cdvd_rpc, "cdvd-scmd2");
    ps2_rpc_register(0x80000595u, cdvd_rpc, "cdvd-ncmd");
    ps2_rpc_register(0x80000597u, cdvd_rpc, "cdvd-srch");
    ps2_rpc_register(0x8000059Au, cdvd_rpc, "cdvd-init");
    ps2_rpc_register(0x8000059Cu, cdvd_ready_rpc, "cdvd-rdy");
}

void ps2_cdvd_report(void) {
    ps2_log("cdvd: %llu reads (%llu sectors, %.1f MB), %llu searches "
            "(%llu misses)",
            (unsigned long long)cd_reads,
            (unsigned long long)cd_sectors_read,
            (double)cd_sectors_read * CDVD_SECTOR / (1024.0 * 1024.0),
            (unsigned long long)cd_searches,
            (unsigned long long)cd_search_misses);
}
