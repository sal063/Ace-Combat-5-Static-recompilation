#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_vfs.h"
#include <string.h>

void hle_sceSifAllocIopHeap(ps2_ctx *);
void hle_sceSifFreeIopHeap(ps2_ctx *);
void hle_sceSifAllocSysMemory(ps2_ctx *);
void hle_sceSifQueryMaxFreeMemSize(ps2_ctx *);
void hle_sceSifQueryTotalFreeMemSize(ps2_ctx *);
void hle_scePad2Init(ps2_ctx *);
void hle_scePad2End(ps2_ctx *);
void hle_scePad2CreateSocket(ps2_ctx *);
void hle_scePad2DeleteSocket(ps2_ctx *);
void hle_sceVibGetProfile(ps2_ctx *);
void ps2_iop_services_register(void);

static u32 invoke(void (*fn)(ps2_ctx *), u32 a, u32 b, u32 c) {
    ps2_ctx ctx = {0};
    ctx.r[4].ud[0] = a;
    ctx.r[5].ud[0] = b;
    ctx.r[6].ud[0] = c;
    fn(&ctx);
    return ctx.r[2].uw[0];
}

int ps2_hle_selftest(const char *disc) {
    int fail = 0;
    const u32 send = 0x10000, recv = 0x11000;
    ps2_sif_hle_init();
    ps2_cdvd_init();
    ps2_cdvd_rpc_register();
    ps2_iop_services_register();
    ps2_pad_init();
    u32 capacity = invoke(hle_sceSifQueryTotalFreeMemSize, 0, 0, 0);
    u32 first = invoke(hle_sceSifAllocIopHeap, 513, 0, 0);
    fail += !first;
    fail += invoke(hle_sceSifFreeIopHeap, first + 256, 0, 0) != (u32)-1;
    fail += invoke(hle_sceSifFreeIopHeap, first, 0, 0) != 0;
    fail += invoke(hle_sceSifFreeIopHeap, first, 0, 0) != (u32)-1;
    for (int i = 0; i < 64; i++) {
        u32 p = invoke(hle_sceSifAllocIopHeap, capacity, 0, 0);
        fail += p != first;
        fail += invoke(hle_sceSifAllocIopHeap, 1, 0, 0) != 0;
        fail += invoke(hle_sceSifFreeIopHeap, p, 0, 0) != 0;
    }
    fail += invoke(hle_sceSifAllocIopHeap, 0xffffffffu, 0, 0) != 0;
    u32 fixed = first + 512;
    fail += invoke(hle_sceSifAllocSysMemory, 2, 256, fixed) != fixed;
    fail += invoke(hle_sceSifAllocSysMemory, 2, 256, fixed) != 0;
    fail += invoke(hle_sceSifAllocSysMemory, 2, 256, fixed + 1) != 0;
    fail += invoke(hle_sceSifQueryMaxFreeMemSize, 0, 0, 0) != capacity - 768;
    fail += invoke(hle_sceSifQueryTotalFreeMemSize, 0, 0, 0) != capacity - 256;
    fail += invoke(hle_sceSifAllocSysMemory, 1, 256, 0) != first + capacity - 256;
    ps2_sif_hle_init();
    ps2_cdvd_rpc_register();
    ps2_iop_services_register();
    fail += invoke(hle_scePad2Init, 0, 0, 0) != 1;
    for (int i = 0; i < 16; i++)
        fail += invoke(hle_scePad2CreateSocket, 0, 0, 0) != (u32)i;
    fail += invoke(hle_scePad2CreateSocket, 0, 0, 0) != (u32)-1;
    fail += invoke(hle_scePad2End, 0, 0, 0) != 1;
    fail += invoke(hle_scePad2Init, 0, 0, 0) != 1;
    fail += invoke(hle_scePad2CreateSocket, 0, 0, 0) != 0;
    ps2_w8(recv, 0xff);
    fail += invoke(hle_sceVibGetProfile, 0, recv, 0) != 1;
    fail += ps2_r8(recv) != 0;
    fail += invoke(hle_scePad2DeleteSocket, 0, 0, 0) != 1;
    fail += invoke(hle_scePad2DeleteSocket, 0, 0, 0) != (u32)-1;
    ps2_w8(recv + 5, 0xa5);
    fail += ps2_rpc_call(0x80000001u, 0, 0, 0, recv, 5) != 0;
    fail += ps2_r8(recv + 5) != 0xa5;
    fail += ps2_rpc_call(0x80000593u, 3, 0, 0, recv, 5) != 0;
    fail += ps2_r32(recv) != 0 || ps2_r8(recv + 5) != 0xa5;
    if (disc) {
        fail += ps2_vfs_open(disc) != 0;
        fail += ps2_rpc_call(0x80000593u, 3, 0, 0, recv, 4) != 0;
        fail += ps2_r32(recv) != 0x14;
        const char path[] = "cd:\\BIN\\RADIOEE.PAC";
        ps2_w32(send + 0x18, sizeof(path));
        ps2_put_mem(send + 0x1c, path, sizeof(path));
        ps2_rpc_call(0x4e554649u, 1, send, 64, recv, 4);
        u32 fd = ps2_r32(recv);
        fail += fd == (u32)-1;
        ps2_w32(send + 8, fd);
        ps2_w32(send + 12, 100);
        ps2_w32(send + 16, 0);
        ps2_rpc_call(0x4e554649u, 5, send, 32, recv, 4);
        fail += ps2_r32(recv) != 100;
        ps2_w32(send + 12, (u32)-40);
        ps2_w32(send + 16, 1);
        ps2_rpc_call(0x4e554649u, 5, send, 32, recv, 4);
        fail += ps2_r32(recv) != 60;
        ps2_w32(send + 16, 2);
        ps2_rpc_call(0x4e554649u, 5, send, 32, recv, 4);
        const ps2_disc_file *f = ps2_vfs_find(path);
        fail += !f || ps2_r32(recv) != f->size - 40;
        ps2_w32(send + 16, 0);
        ps2_rpc_call(0x4e554649u, 5, send, 32, recv, 4);
        fail += ps2_r32(recv) != (u32)-1;
    }
    ps2_log("HLE contract regressions: %s (%d failures)", fail ? "FAILED" : "passed", fail);
    return fail != 0;
}
