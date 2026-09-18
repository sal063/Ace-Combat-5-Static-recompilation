#include "ps2_runtime.h"
#include "rn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rn_dump_on;

static FILE *df;
static long target = -1;
static u64 fields;

static struct {
    int live;
    u32 cmd, base, elems, cl, wl;
    const u8 *mem;
} up;

static u32 vtx_run, vtx_regs[16];

static void flush_vtx(void) {
    if (!vtx_run || !df) return;
    fprintf(df, "G   %u vertex writes (", vtx_run);
    for (int i = 0; i < 16; i++)
        if (vtx_regs[i]) fprintf(df, " %X:%u", i, vtx_regs[i]);
    fprintf(df, " )\n");
    vtx_run = 0;
    memset(vtx_regs, 0, sizeof vtx_regs);
}

static const char *unpack_type(u32 t) {
    static char b[16];
    static const u32 bits[4] = { 32, 16, 8, 5 };
    snprintf(b, sizeof b, "V%u-%u", ((t >> 2) & 3u) + 1u, bits[t & 3u]);
    return b;
}

static void flush_unpack(void) {
    u32 show;
    if (!up.live || !df) return;
    up.live = 0;
    if (!up.mem) return;
    show = up.elems < 6u ? up.elems : 6u;
    for (u32 i = 0; i < show; i++) {
        u32 a = (up.base + i) & 0x3FFu;
        float f[4];
        u32 w[4];
        memcpy(f, up.mem + a * 16u, 16);
        memcpy(w, up.mem + a * 16u, 16);
        fprintf(df, "V     [%03X] %g %g %g %g   (%08X %08X %08X %08X)\n", a,
                f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
    if (up.elems > show) fprintf(df, "V     ... %u more\n", up.elems - show);
}

void rn_dump_init(void) {
    const char *p = getenv("PS2_RN_DUMP");
    const char *n = getenv("PS2_RN_DUMP_FIELD");
    if (!p || !*p) return;
    df = fopen(p, "w");
    if (!df) return;
    target = n ? strtol(n, NULL, 0) : 10;
    rn_dump_on = target == 0;
    ps2_log("rn: dumping field %ld of the graphics stream to %s", target, p);
}

void rn_dump_frame(void) {
    if (!df) return;
    if (rn_dump_on) {
        flush_vtx();
        flush_unpack();
        fprintf(df, "=== end of field %llu\n", (unsigned long long)fields);
        fflush(df);
    }
    fields++;
    rn_dump_on = (long)fields == target;
    if (rn_dump_on) fprintf(df, "=== field %llu\n", (unsigned long long)fields);
}

void rn_dump_tag(u32 kind, u32 a, u32 b) {
    (void)b;
    if (kind != RN_TAG_EMITTER) return;
    flush_vtx();
    flush_unpack();
    {
        const rn_emitter *e = rn_emitter_get(a);
        fprintf(df, "E %u site %08X via %u\n", a, e ? e->site : 0u, e ? e->via : 0u);
    }
}

static struct { int live; u32 addr, n; const u8 *micro; } mpg;

static void flush_mpg(void) {
    u32 w[4];
    if (!mpg.live || !mpg.micro) { mpg.live = 0; return; }
    mpg.live = 0;
    memcpy(w, mpg.micro + mpg.addr, 16);
    fprintf(df, "V     %04X: %08X %08X %08X %08X\n", mpg.addr, w[0], w[1], w[2], w[3]);
}

void rn_dump_vif(u32 code, u32 tops, u32 cl, u32 wl, u32 tpc, const u8 *mem,
                 const u8 *micro) {
    u32 cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu, imm = code & 0xFFFFu;
    flush_vtx();
    flush_unpack();
    flush_mpg();
    if (cmd == 0x4A) {
        mpg.live = 1;
        mpg.addr = imm * 8u;
        mpg.n = num ? num : 256u;
        mpg.micro = micro;
    }
    if (cmd >= 0x60) {
        u32 base = (imm & 0x3FFu) + (((imm >> 15) & 1u) ? tops : 0u);
        up.live = 1;
        up.cmd = cmd;
        up.base = base;
        up.elems = num ? num : 256u;
        up.cl = cl;
        up.wl = wl;
        up.mem = mem;
        fprintf(df, "V UNPACK %s%s%s -> %03X x%u cycle %u/%u\n",
                unpack_type(cmd & 0xFu), (cmd & 0x10u) ? " masked" : "",
                ((imm >> 14) & 1u) ? " unsigned" : "", base, up.elems, cl, wl);
        return;
    }
    switch (cmd) {
    case 0x00: return;
    case 0x01: fprintf(df, "V STCYCL %u/%u\n", imm & 0xFFu, (imm >> 8) & 0xFFu); return;
    case 0x02: fprintf(df, "V OFFSET %03X\n", imm & 0x3FFu); return;
    case 0x03: fprintf(df, "V BASE %03X\n", imm & 0x3FFu); return;
    case 0x04: fprintf(df, "V ITOP %03X\n", imm & 0x3FFu); return;
    case 0x05: fprintf(df, "V STMOD %u\n", imm & 3u); return;
    case 0x10: fprintf(df, "V FLUSHE\n"); return;
    case 0x11: fprintf(df, "V FLUSH\n"); return;
    case 0x13: fprintf(df, "V FLUSHA\n"); return;
    case 0x14: fprintf(df, "V MSCAL %04X\n", imm * 8u); return;
    case 0x15: fprintf(df, "V MSCALF %04X\n", imm * 8u); return;
    case 0x17: fprintf(df, "V MSCNT (from %04X)\n", tpc); return;
    case 0x4A: fprintf(df, "V MPG %04X x%u\n", imm * 8u, num ? num : 256u); return;
    case 0x50: case 0x51:
        fprintf(df, "V DIRECT%s %u qw\n", cmd == 0x51 ? "HL" : "", imm ? imm : 65536u);
        return;
    default:
        fprintf(df, "V code %08X\n", code);
        return;
    }
}

void rn_dump_xgkick(u32 addr, const u8 *mem, u32 mem_size) {
    u64 tag, regs;
    u32 a = addr & 0x3FFu;
    flush_vtx();
    flush_unpack();
    if (!mem || a * 16u + 16u > mem_size) {
        fprintf(df, "K XGKICK %03X\n", a);
        return;
    }
    memcpy(&tag, mem + a * 16u, 8);
    memcpy(&regs, mem + a * 16u + 8u, 8);
    fprintf(df, "K XGKICK %03X nloop %u eop %u pre %u prim %03X nreg %u regs %016llX\n",
            a, (u32)(tag & 0x7FFFu), (u32)((tag >> 15) & 1u),
            (u32)((tag >> 46) & 1u), (u32)((tag >> 47) & 0x7FFu),
            ((tag >> 60) & 0xFu) ? (u32)((tag >> 60) & 0xFu) : 16u,
            (unsigned long long)regs);
}

void rn_dump_gsreg(u32 reg, u64 val) {
    switch (reg) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x0A:
    case 0x0C: case 0x0D:
        vtx_run++;
        vtx_regs[reg & 15]++;
        return;
    default:
        break;
    }
    flush_vtx();
    flush_unpack();
    fprintf(df, "G %02X = %016llX\n", reg, (unsigned long long)val);
}
