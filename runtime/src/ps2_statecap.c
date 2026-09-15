#include "ps2_statecap.h"
#include "ps2_capture.h"
#include "ps2_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#ifdef _WIN32
#  include <windows.h>
#endif

static u8      *g_ram;
static u64      g_size;
static char     g_dir[260];
static char     g_why[128];
static int      g_seq;
static volatile int g_busy;
static volatile int g_gspend;
static FILE    *g_man;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int             g_armed;
static int             g_up;

#define NPROV 12
static struct { const char *name; void (*fn)(const char *); } g_prov[NPROV];
static int g_nprov;

const char *ps2_statecap_dir(void)     { return g_dir; }
int  ps2_statecap_gs_pending(void)     { return g_gspend != 0; }
void ps2_statecap_gs_done(void)        { g_gspend = 0; }

void ps2_statecap_register(const char *name, void (*fn)(const char *dir)) {
    if (g_nprov < NPROV) {
        g_prov[g_nprov].name = name;
        g_prov[g_nprov].fn = fn;
        g_nprov++;
    }
}

static void man(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_man) { vfprintf(g_man, fmt, ap); fputc('\n', g_man); }
    va_end(ap);
}

static unsigned long now_ms(void) {
    return (unsigned long)(ps2_wall_seconds() * 1000.0);
}

static void snap_sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int dump_ram(const char *dir) {
    char p[300];
    FILE *f;
    const u32 shift = 16, bsz = 1u << 16;
    u32 nblk, n = 0, b, i;
    u32 *ids;
    u8 hdr[32];

    snprintf(p, sizeof p, "%s/ram.bin", dir);
    f = fopen(p, "wb");
    if (!f) { man("ram.bin: FAILED to open"); return 0; }
    nblk = (u32)(g_size >> shift);
    ids = (u32 *)malloc((size_t)nblk * 4u);
    if (!ids) { fclose(f); man("ram.bin: out of memory"); return 0; }
    for (b = 0; b < nblk; b++) {
        const u64 *q = (const u64 *)(g_ram + ((u64)b << shift));
        for (i = 0; i < bsz / 8u; i++) if (q[i]) { ids[n++] = b; break; }
    }
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "PS2RAM01", 8);
    memcpy(hdr + 8, &shift, 4);
    memcpy(hdr + 12, &nblk, 4);
    memcpy(hdr + 16, &n, 4);
    memcpy(hdr + 24, &g_size, 8);
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(ids, 4, n, f);
    for (i = 0; i < n; i++)
        fwrite(g_ram + ((u64)ids[i] << shift), 1, bsz, f);
    fclose(f);
    man("ram.bin        guest RAM, %u/%u blocks of 64KB present (%.0f MB), "
        "address space 0x%llx", n, nblk, (double)n * bsz / 1048576.0,
        (unsigned long long)g_size);
    free(ids);
    return 1;
}

static void dump_flat(const char *dir, const char *name, const void *p, u64 n) {
    char q[300];
    FILE *f;
    if (!p || !n) { man("%-14s (not present)", name); return; }
    snprintf(q, sizeof q, "%s/%s", dir, name);
    f = fopen(q, "wb");
    if (!f) { man("%-14s FAILED to open", name); return; }
    fwrite(p, 1, (size_t)n, f);
    fclose(f);
    man("%-14s %llu bytes, flat", name, (unsigned long long)n);
}

static void write_reader(const char *dir) {
    char p[300];
    FILE *f;
    snprintf(p, sizeof p, "%s/read_ram.py", dir);
    f = fopen(p, "w");
    if (!f) return;
    fputs(
"#!/usr/bin/env python3\n"
"\"\"\"read_ram.py - random access into a statecap ram.bin.\n"
"\n"
"  python read_ram.py ram.bin 0x00100000 256      # hexdump 256 bytes at an address\n"
"  python read_ram.py ram.bin --blocks            # list present 64KB blocks\n"
"\n"
"As a module:  Ram('ram.bin').read(addr, n) -> bytes (zeros where nothing was\n"
"present, i.e. that block was all zero when the snapshot was taken).\n"
"\n"
"Main memory is mapped linearly on the PS2, so an offset in this file IS a\n"
"guest address: anything read out of a capture can be checked against an\n"
"address in IDA without a page table in the middle.  Guest words are\n"
"little-endian, unlike the PS3's.\n"
"\"\"\"\n"
"import struct, sys\n"
"\n"
"class Ram:\n"
"    def __init__(self, path):\n"
"        self.f = open(path, 'rb')\n"
"        h = self.f.read(32)\n"
"        assert h[:8] == b'PS2RAM01', 'not a statecap ram.bin'\n"
"        self.shift, self.nblk, self.npres = struct.unpack_from('<III', h, 8)\n"
"        self.mem_size, = struct.unpack_from('<Q', h, 24)\n"
"        self.bsz = 1 << self.shift\n"
"        ids = struct.unpack('<%dI' % self.npres, self.f.read(4 * self.npres))\n"
"        self.data0 = 32 + 4 * self.npres\n"
"        self.idx = {b: i for i, b in enumerate(ids)}\n"
"        self.blocks = ids\n"
"    def read(self, addr, n):\n"
"        addr &= 0x1FFFFFFF          # strip kseg0/kseg1, as the EE does\n"
"        out = bytearray()\n"
"        while n > 0:\n"
"            b, off = addr >> self.shift, addr & (self.bsz - 1)\n"
"            take = min(n, self.bsz - off)\n"
"            i = self.idx.get(b)\n"
"            if i is None:\n"
"                out += b'\\x00' * take\n"
"            else:\n"
"                self.f.seek(self.data0 + i * self.bsz + off)\n"
"                out += self.f.read(take)\n"
"            addr += take; n -= take\n"
"        return bytes(out)\n"
"    def u32(self, addr):   # guest words are little-endian\n"
"        return struct.unpack('<I', self.read(addr, 4))[0]\n"
"    def u64(self, addr):\n"
"        return struct.unpack('<Q', self.read(addr, 8))[0]\n"
"    def f32(self, addr):\n"
"        return struct.unpack('<f', self.read(addr, 4))[0]\n"
"\n"
"if __name__ == '__main__':\n"
"    r = Ram(sys.argv[1])\n"
"    if len(sys.argv) > 2 and sys.argv[2] == '--blocks':\n"
"        print('%d blocks of %d bytes' % (r.npres, r.bsz))\n"
"        for b in r.blocks: print('0x%08x' % (b << r.shift))\n"
"        raise SystemExit\n"
"    a = int(sys.argv[2], 0); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 256\n"
"    d = r.read(a, n)\n"
"    for o in range(0, len(d), 16):\n"
"        row = d[o:o+16]\n"
"        print('%08x  %-48s %s' % (a + o, ' '.join('%02x' % c for c in row),\n"
"              ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))\n", f);
    fclose(f);
}

static void take(const char *why) {
    time_t tt = time(NULL);
    struct tm lt;
    char p[300];
    unsigned long t0, r0;
    int i;

#ifdef _WIN32
    { struct tm *q = localtime(&tt); lt = q ? *q : (struct tm){0}; }
#else
    localtime_r(&tt, &lt);
#endif
    snprintf(g_dir, sizeof g_dir, "out/snap_%03d_%02d%02d%02d", ++g_seq,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    ps2_mkdir_p("out");
    ps2_mkdir_p(g_dir);
    snprintf(p, sizeof p, "%s/manifest.txt", g_dir);
    g_man = fopen(p, "w");
    t0 = now_ms();
    man("statecap snapshot");
    man("  reason      %s", why && why[0] ? why : "(unspecified)");
    man("  taken       %04d-%02d-%02d %02d:%02d:%02d local",
        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
        lt.tm_hour, lt.tm_min, lt.tm_sec);
    man("  uptime      %lu ms", t0);
    man("  built       " __DATE__ " " __TIME__);
    man("");
    man("The guest was NOT stopped while this was written; each line below ends");
    man("with the millisecond it finished, so the skew between parts is visible.");
    man("");

    g_gspend = 1;

    write_reader(g_dir);
    if (!PS2_ENV("PS2_SNAP_NORAM")) {
        dump_ram(g_dir);
        man("               (RAM done at %lu ms, %lu ms after the trigger)",
            now_ms(), now_ms() - t0);
    } else {
        man("ram.bin        SKIPPED (PS2_SNAP_NORAM)");
    }
    dump_flat(g_dir, "spr.bin", ps2_spr, ps2_spr ? PS2_SPR_SIZE : 0u);
    dump_flat(g_dir, "iop_ram.bin", ps2_iop_ram,
              ps2_iop_ram ? PS2_IOP_RAM_SIZE : 0u);

    for (i = 0; i < g_nprov; i++) {
        unsigned long a = now_ms();
        g_prov[i].fn(g_dir);
        man("%-14s provider done at %lu ms (+%lu)", g_prov[i].name,
            now_ms(), now_ms() - a);
    }

    if (!PS2_ENV("PS2_SNAP_NOCHUNK")) {
        unsigned long a = now_ms();
        snprintf(p, sizeof p, "%s/machine.bin", g_dir);
        ps2_state_dump(p);
        man("machine.bin    ps2_state chunk dump at %lu ms (+%lu)", now_ms(),
            now_ms() - a);
    }

    r0 = now_ms();
    for (i = 0; i < 300 && g_gspend; i++) snap_sleep_ms(10);
    if (!g_gspend) {
        man("gs             registers + targets written at the first frame after");
        man("               the trigger, %lu ms into the snapshot (%lu ms waiting)",
            now_ms() - t0, now_ms() - r0);
    } else {
        g_gspend = 0;
        man("gs             NO FRAME within 3s -- the GS-side files are missing.");
        man("               That is itself a finding: the presenting thread was");
        man("               not reaching a frame while this snapshot was taken.");
    }
    man("");
    man("total          %lu ms", now_ms() - t0);
    fclose(g_man); g_man = 0;
    ps2_log("snap: wrote %s (%lu ms)", g_dir, now_ms() - t0);
}

static void *snap_thread(void *p) {
    (void)p;
    for (;;) {
        char why[128];
        pthread_mutex_lock(&g_mx);
        while (!g_armed) pthread_cond_wait(&g_cv, &g_mx);
        g_armed = 0;
        memcpy(why, g_why, sizeof why);
        why[sizeof why - 1] = 0;
        pthread_mutex_unlock(&g_mx);
        take(why);
        g_busy = 0;
    }
    return NULL;
}

void ps2_statecap_request(const char *why) {
    if (!g_up) return;
    pthread_mutex_lock(&g_mx);
    if (g_busy) { pthread_mutex_unlock(&g_mx); return; }
    g_busy = 1;
    snprintf(g_why, sizeof g_why, "%s", why ? why : "");
    g_armed = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
    ps2_log("snap: requested: %s", why ? why : "");
}

static void *marker_thread(void *p) {
    (void)p;
    for (;;) {
        FILE *f;
        snap_sleep_ms(200);
        f = fopen("snap.on", "rb");
        if (f) {
            char b[96];
            size_t r = fread(b, 1, sizeof b - 1, f);
            size_t i;
            static char sb[104];
            b[r] = 0;
            fclose(f);
            remove("snap.on");
            for (i = 0; i < r; i++) if (b[i] == '\n' || b[i] == '\r') b[i] = 0;
            snprintf(sb, sizeof sb, "snap.on %s", b);
            ps2_statecap_request(sb);
        }
    }
    return NULL;
}

void ps2_statecap_init(u8 *ram, u64 size) {
    pthread_t th;
    g_ram = ram; g_size = size;
    g_up = 1;
    pthread_create(&th, NULL, snap_thread, NULL);
    pthread_detach(th);
    if (!PS2_ENV("PS2_SNAP_NOKEY")) {
        pthread_create(&th, NULL, marker_thread, NULL);
        pthread_detach(th);
    }
    ps2_log("snap: armed: press F8 in the game window (or drop snap.on) for a "
            "full state snapshot into out/snap_NNN/");
}
