#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_vfs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8 *ps2_pt[PS2_PT_ENTRIES];
u8 *ps2_ram;
static int quiet;
void ps2_log(const char *fmt, ...) {
    va_list ap;
    if (quiet) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
void ps2_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(2);
}
u8   ps2_mmio_r8(u32 a) { (void)a; abort(); }
u16  ps2_mmio_r16(u32 a) { (void)a; abort(); }
u32  ps2_mmio_r32(u32 a) { (void)a; abort(); }
u64  ps2_mmio_r64(u32 a) { (void)a; abort(); }
void ps2_mmio_r128(ps2_reg128 *d, u32 a) { (void)d; (void)a; abort(); }
void ps2_mmio_w8(u32 a, u8 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w16(u32 a, u16 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w32(u32 a, u32 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w64(u32 a, u64 v) { (void)a; (void)v; abort(); }
void ps2_mmio_w128(u32 a, const ps2_reg128 *v) { (void)a; (void)v; abort(); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u32 al16(u32 n) { return (n + 15u) & ~15u; }

typedef struct { u32 off, packed, unpacked; } tbl_entry;
static tbl_entry *disc_tbl;
static u32 disc_count;

static void read_disc_tbl(void) {
    const ps2_disc_file *f = ps2_vfs_find("BIN/DATA.TBL");
    u8 *t;
    if (!f) { printf("no DATA.TBL\n"); exit(2); }
    t = (u8 *)malloc(f->size);
    ps2_vfs_read(f, 0, f->size, t);
    disc_count = rd32(t);
    disc_tbl = (tbl_entry *)malloc(disc_count * sizeof *disc_tbl);
    for (u32 i = 0; i < disc_count; i++) {
        disc_tbl[i].off = rd32(t + 8 + 8 * i);
        disc_tbl[i].packed = rd32(t + 12 + 8 * i);
        disc_tbl[i].unpacked = rd32(t + 8 + 8 * disc_count + 4 * i);
    }
    free(t);
}

static u8 *disc_member(u32 m, u32 *len) {
    const ps2_disc_file *pac = ps2_vfs_find("BIN/DATA.PAC");
    u8 *packed = (u8 *)malloc(disc_tbl[m].packed);
    u8 *out = (u8 *)malloc(disc_tbl[m].unpacked);
    ps2_vfs_read(pac, disc_tbl[m].off, disc_tbl[m].packed, packed);
    if (ps2_vfs_ulz_decode(packed, disc_tbl[m].packed, out, disc_tbl[m].unpacked)
        != (s64)disc_tbl[m].unpacked) {
        free(packed); free(out);
        return NULL;
    }
    free(packed);
    *len = disc_tbl[m].unpacked;
    return out;
}

static const u8 *file_in(const u8 *mem, u32 len, u32 k, u32 *flen) {
    u32 n = rd32(mem), start = rd32(mem + 4 + 4 * k), next = len;
    if (!start) { *flen = 0; return NULL; }
    for (u32 j = 0; j < n; j++) {
        u32 o = rd32(mem + 4 + 4 * j);
        if (o > start && o < next) next = o;
    }
    *flen = next - start;
    return mem + start;
}

static int cmd_identity(void) {
    u32 bad = 0;
    quiet = 1;
    read_disc_tbl();
    for (u32 m = 0; m < disc_count; m++) {
        u32 dlen, clen;
        u8 *d = disc_member(m, &dlen), *c = NULL;
        if (!d) { CHECK(0, "member %u does not decode", m); bad++; continue; }
        if (ps2_vfs_pac_compose(m, &c, &clen) != 0) {
            CHECK(0, "member %u does not compose", m);
            bad++;
        } else if (clen != dlen || memcmp(c, d, dlen)) {
            CHECK(0, "member %u rebuilt differs (%u vs %u bytes)", m, clen, dlen);
            bad++;
        }
        free(c);
        free(d);
    }
    printf("identity: %u of %u members rebuilt byte for byte\n",
           disc_count - bad, disc_count);
    return fails != 0;
}

static u32 crc32_of(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int cmd_digest(int argc, char **argv) {
    static u8 buf[1 << 16];
    quiet = 1;
    for (int i = 3; i < argc; i++) {
        const ps2_disc_file *f = ps2_vfs_find(argv[i]);
        u32 head, tail, n;
        if (!f) { printf("%s missing\n", argv[i]); continue; }
        n = f->size < sizeof buf ? f->size : (u32)sizeof buf;
        ps2_vfs_read(f, 0, n, buf);
        head = crc32_of(buf, n);
        ps2_vfs_read(f, f->size - n, n, buf);
        tail = crc32_of(buf, n);
        {
            u8 sec[2048 * 2];
            u32 last = (f->size ? (f->size - 1) / 2048 : 0);
            ps2_vfs_read_sectors(f->lsn + last, 2, sec);
            printf("%s size=%u head=%08X tail=%08X lastsec=%08X\n", argv[i],
                   f->size, head, tail, crc32_of(sec, 2048));
        }
    }
    read_disc_tbl();
    for (u32 m = 0; m < disc_count; m += 97) {
        u8 *c;
        u32 clen;
        if (ps2_vfs_pac_compose(m, &c, &clen) == 0) {
            printf("member %u len=%u crc=%08X\n", m, clen, crc32_of(c, clen));
            free(c);
        }
    }
    return 0;
}

static u8 *slurp(const char *path, u32 *len) {
    FILE *fp = fopen(path, "rb");
    long n;
    u8 *b;
    if (!fp) { printf("cannot open %s\n", path); exit(2); }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    b = (u8 *)malloc(n ? (size_t)n : 1u);
    if (fread(b, 1, (size_t)n, fp) != (size_t)n) exit(2);
    fclose(fp);
    *len = (u32)n;
    return b;
}

typedef struct { const char *path, *file, *owner; int priority; } claim_spec;

static const claim_spec specs[] = {
    { "select/noise.gim",                     "noise_a.bin",  "a_plain",  0 },
    { "select/noise.gim",                     "noise_hi.bin", "hi",       5 },
    { "SELECT/NOISE.GIM",                     "noise_lo.bin", "lo",      -1 },
    { "BIN/DATA.PAC/0011/map/m09a/m09a.wad",  "wad.bin",      "a_plain",  0 },
    { "mistitle/backlight.gim",               "light_c.bin",  "a_plain",  0 },
    { "BIN/DATA.PAC/%s/mistitle/backlight.gim", "light_s.bin", "b_scoped", 0 },
    { "BIN/DATA.PAC/0000/#3",                 "index3.bin",   "a_plain",  0 },
    { "bin\\us\\bgm.pac",                     "bgm.bin",      "a_plain",  0 },
    { "mymod/data.bin",                       "new.bin",      "a_plain",  0 },
    { "BIN/DATA.PAC/9999/x.bin",              "new.bin",      "bad",      0 },
    { "BIN/DATA.PAC/0000/#999",               "new.bin",      "bad",      0 },
    { "dummy.bin",                            "dummy16.bin",  "a_plain",  0 },
};

static int cmd_mods(const char *disc, const char *work, const char *light_member) {
    char host[1024], scoped[128];
    u32 nspec = sizeof specs / sizeof specs[0];
    u8 *want[16];
    u32 wlen[16];

    for (u32 i = 0; i < nspec; i++) {
        const char *path = specs[i].path;
        if (strstr(path, "%s")) {
            snprintf(scoped, sizeof scoped, path, light_member);
            path = scoped;
        }
        snprintf(host, sizeof host, "%s/%s", work, specs[i].file);
        want[i] = slurp(host, &wlen[i]);
        ps2_vfs_claim(path, host, wlen[i], specs[i].priority, specs[i].owner);
    }
    if (ps2_vfs_open(disc) != 0) { printf("cannot open %s\n", disc); return 2; }

    {
        FILE *probe;
        char tbl[1024];
        u8 *t;
        u32 tl;
        snprintf(tbl, sizeof tbl, "%s/tbl.bin", work);
        probe = fopen(tbl, "rb");
        if (!probe) { printf("test_vfs.py did not write tbl.bin\n"); return 2; }
        fclose(probe);
        t = slurp(tbl, &tl);
        disc_count = rd32(t);
        disc_tbl = (tbl_entry *)malloc(disc_count * sizeof *disc_tbl);
        for (u32 i = 0; i < disc_count; i++) {
            disc_tbl[i].off = rd32(t + 8 + 8 * i);
            disc_tbl[i].packed = rd32(t + 12 + 8 * i);
            disc_tbl[i].unpacked = rd32(t + 8 + 8 * disc_count + 4 * i);
        }
        free(t);
    }

    {
        u32 dlen, clen, flen, olen;
        u8 *d = disc_member(0, &dlen), *c;
        CHECK(ps2_vfs_pac_is_changed(0), "member 0 should be changed");
        CHECK(ps2_vfs_pac_compose(0, &c, &clen) == 0, "member 0 compose");
        CHECK(rd32(c) == rd32(d), "member 0 file count");
        {
            const u8 *got = file_in(c, clen, 10, &flen);
            CHECK(got && flen == al16(wlen[1]) && !memcmp(got, want[1], wlen[1]),
                  "noise.gim should be the priority-5 file (%u bytes)", flen);
        }
        {
            const u8 *got = file_in(c, clen, 3, &flen);
            CHECK(got && flen == al16(wlen[6]) && !memcmp(got, want[6], wlen[6]),
                  "#3 should be replaced");
        }
        for (u32 k = 0; k < rd32(d); k++) {
            const u8 *a, *b;
            if (k == 10 || k == 3) continue;
            a = file_in(d, dlen, k, &olen);
            b = file_in(c, clen, k, &flen);
            CHECK(olen == flen && (!a || !memcmp(a, b, olen)),
                  "member 0 file %u should be untouched", k);
        }
        CHECK(ps2_vfs_pac_size(0) == clen, "member 0 size");
        free(c);
        free(d);
    }

    {
        u32 dlen, clen, flen, olen, absent = 0;
        u8 *d = disc_member(11, &dlen), *c;
        CHECK(ps2_vfs_pac_compose(11, &c, &clen) == 0, "member 11 compose");
        for (u32 k = 0; k < rd32(d); k++) {
            const u8 *a = file_in(d, dlen, k, &olen), *b = file_in(c, clen, k, &flen);
            if (k == 25) {
                CHECK(b && flen == al16(wlen[3]) && !memcmp(b, want[3], wlen[3]),
                      "m09a.wad should be the mod's");
                continue;
            }
            if (k == 36 || k == 37) {
                CHECK(b && flen == 16 && !memcmp(b, want[11], 16),
                      "member 11 file %u should be the dummy.bin claim's", k);
                continue;
            }
            if (!a) {
                absent++;
                CHECK(rd32(c + 4 + 4 * k) == 0, "member 11 file %u must stay absent", k);
                continue;
            }
            CHECK(b && olen == flen && !memcmp(a, b, olen),
                  "member 11 file %u should be untouched", k);
        }
        CHECK(absent > 0, "member 11 was chosen for its absent entries");
        free(c);
        free(d);
    }

    {
        u32 scoped_m = (u32)strtoul(light_member, NULL, 10), seen = 0;
        for (u32 m = 0; m < disc_count; m++) {
            char path[128];
            u32 which = m == scoped_m ? 5 : 4;
            u8 *b;
            s64 sz;
            snprintf(path, sizeof path, "BIN/DATA.PAC/%04u/mistitle/backlight.gim", m);
            sz = ps2_vfs_stat(path);
            if (sz < 0) continue;
            seen++;
            CHECK(ps2_vfs_pac_is_changed(m), "member %u holds backlight.gim", m);
            CHECK(sz == (s64)wlen[which], "member %u backlight.gim is %lld bytes",
                  m, (long long)sz);
            b = (u8 *)malloc(wlen[which]);
            CHECK(ps2_vfs_read_path(path, 0, wlen[which], b) == (s64)wlen[which]
                  && !memcmp(b, want[which], wlen[which]),
                  "member %u backlight.gim should be the %s claim's", m,
                  which == 5 ? "scoped" : "plain");
            free(b);
        }
        CHECK(seen == 33, "backlight.gim should be in 33 members, saw %u", seen);
    }

    {
        u32 clen, flen;
        u8 *c;
        CHECK(ps2_vfs_pac_compose(157, &c, &clen) == 0, "member 157 compose");
        CHECK(rd32(c + 4 + 4 * 21) == 0, "member 157 file 21 must stay absent");
        free(c);
        CHECK(ps2_vfs_pac_compose(3, &c, &clen) == 0, "member 3 compose");
        {
            const u8 *b = file_in(c, clen, 36, &flen);
            CHECK(b && flen == 16 && !memcmp(b, want[11], 16), "member 3 dummy.bin");
        }
        free(c);
    }

    {
        const ps2_disc_file *f = ps2_vfs_find("cdrom0:\\BIN\\DATA.TBL;1");
        u8 *t = (u8 *)malloc(f->size);
        u32 changed = 0;
        ps2_vfs_read(f, 0, f->size, t);
        for (u32 m = 0; m < disc_count; m++) {
            CHECK(rd32(t + 8 + 8 * m) == disc_tbl[m].off, "tbl offset %u", m);
            CHECK(rd32(t + 12 + 8 * m) == disc_tbl[m].packed, "tbl packed %u", m);
            CHECK(rd32(t + 8 + 8 * disc_count + 4 * m) == ps2_vfs_pac_size(m),
                  "tbl unpacked %u", m);
            if (ps2_vfs_pac_size(m) != disc_tbl[m].unpacked) changed++;
        }
        CHECK(ps2_vfs_pac_changed() >= 3, "changed members: %u", ps2_vfs_pac_changed());
        printf("mods: %u members changed, %u with a new size\n",
               ps2_vfs_pac_changed(), changed);
        free(t);
    }

    {
        static u8 ram[PS2_RAM_SIZE];
        u32 len = ps2_vfs_pac_size(11);
        u8 *c;
        u32 clen;
        for (u32 p = 0; p < PS2_RAM_SIZE >> PS2_PAGE_BITS; p++)
            ps2_pt[p] = ram + ((size_t)p << PS2_PAGE_BITS);
        CHECK(ps2_vfs_pac_fill_guest(11, 0x00100000u, len - 1) < 0,
              "a short buffer must be refused");
        CHECK(ps2_vfs_pac_fill_guest(11, 0x00100000u, len) == (int)len,
              "fill member 11");
        ps2_vfs_pac_compose(11, &c, &clen);
        CHECK(clen == len && !memcmp(ram + 0x00100000u, c, len), "guest bytes");
        free(c);
        CHECK(ps2_vfs_pac_fill_guest(1, 0x00100000u, 0x01000000u) < 0,
              "an unchanged member is the game's to decode");
    }

    {
        const ps2_disc_file *f = ps2_vfs_find("cd:\\BIN\\US\\BGM.PAC");
        u8 sec[2048 * 4];
        CHECK(f && f->size == wlen[7], "BGM.PAC size %u", f ? f->size : 0);
        if (f) {
            u32 sectors = (f->size + 2047) / 2048;
            u8 *back = (u8 *)malloc(f->size);
            CHECK(ps2_vfs_read(f, 0, f->size, back) == (int)f->size
                  && !memcmp(back, want[7], wlen[7]), "BGM.PAC bytes");
            free(back);
            ps2_vfs_read_sectors(f->lsn, sectors, sec);
            CHECK(!memcmp(sec, want[7], wlen[7]), "BGM.PAC through sectors");
            for (u32 i = wlen[7]; i < sectors * 2048u; i++)
                if (sec[i]) { CHECK(0, "BGM.PAC tail must read as zeros"); break; }
        }
    }

    {
        u8 b[64];
        const ps2_disc_file *f = ps2_vfs_find("MYMOD/DATA.BIN");
        CHECK(f && f->size == wlen[8], "new file");
        CHECK(ps2_vfs_stat("select/noise.gim") == (s64)wlen[1], "stat plain name");
        CHECK(ps2_vfs_stat("BIN/DATA.PAC/0011/map/m09a/m09a.wad") == (s64)wlen[3],
              "stat scoped name");
        CHECK(ps2_vfs_read_path("select/noise.gim", 4, 16, b) == 16
              && !memcmp(b, want[1] + 4, 16), "read plain name");
        CHECK(ps2_vfs_stat("BIN/DATA.PAC/0000/#999") < 0, "a bad index");
    }

    {
        const ps2_disc_file *pac = ps2_vfs_find("BIN/DATA.PAC");
        const ps2_disc_file *tbl = ps2_vfs_find("BIN/DATA.TBL");
        const ps2_disc_file *bgm = ps2_vfs_find("BIN/US/BGM.PAC");
        CHECK(pac->lsn >= 32 && tbl->lsn != pac->lsn, "lsns assigned");
        CHECK(tbl->lsn >= pac->lsn + (pac->size + 2047) / 2048
              || pac->lsn >= tbl->lsn + (tbl->size + 2047) / 2048, "pac/tbl overlap");
        CHECK(bgm->lsn >= tbl->lsn + 1 || bgm->lsn + 1 <= tbl->lsn, "bgm/tbl overlap");
    }

    ps2_vfs_pac_revert("test");
    {
        const ps2_disc_file *f = ps2_vfs_find("BIN/DATA.TBL");
        u8 *t = (u8 *)malloc(f->size);
        ps2_vfs_read(f, 0, f->size, t);
        for (u32 m = 0; m < disc_count; m++)
            CHECK(rd32(t + 8 + 8 * disc_count + 4 * m) == disc_tbl[m].unpacked,
                  "reverted tbl %u", m);
        CHECK(!ps2_vfs_pac_is_changed(0) && !ps2_vfs_pac_changed(), "reverted");
        free(t);
    }
    ps2_vfs_report();
    for (u32 i = 0; i < nspec; i++) free(want[i]);
    return fails != 0;
}

static int cmd_copies(const char *name) {
    u32 distinct[64], ndistinct = 0, copies = 0;
    read_disc_tbl();
    for (u32 m = 0; m < disc_count; m++) {
        char path[300];
        s64 size;
        u8 *b;
        u32 crc, i;
        snprintf(path, sizeof path, "BIN/DATA.PAC/%04u/%s", m, name);
        size = ps2_vfs_stat(path);
        if (size < 0) continue;
        b = (u8 *)malloc((size_t)size + 1u);
        ps2_vfs_read_path(path, 0, (u32)size, b);
        crc = crc32_of(b, (size_t)size) ^ (u32)size;
        free(b);
        copies++;
        for (i = 0; i < ndistinct && distinct[i] != crc; i++) {}
        if (i == ndistinct && ndistinct < 64) {
            distinct[ndistinct++] = crc;
            printf("  first seen in member %u: %lld bytes, crc %08X\n", m,
                   (long long)size, crc);
        }
    }
    printf("copies: '%s' in %u members, %u distinct\n", name, copies, ndistinct);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: vfs_test identity|digest|copies|mods DISC [...]\n");
        return 2;
    }
    if (!strcmp(argv[1], "mods")) {
        if (argc < 5) return 2;
        return cmd_mods(argv[2], argv[3], argv[4]);
    }
    quiet = 1;
    if (ps2_vfs_open(argv[2]) != 0) { printf("cannot open %s\n", argv[2]); return 2; }
    quiet = 0;
    if (!strcmp(argv[1], "identity")) return cmd_identity();
    if (!strcmp(argv[1], "digest")) return cmd_digest(argc, argv);
    if (!strcmp(argv[1], "copies") && argc > 3) return cmd_copies(argv[3]);
    if (!strcmp(argv[1], "cat") && argc > 4) {
        s64 size = ps2_vfs_stat(argv[3]);
        FILE *fp;
        u8 *b;
        if (size < 0) { printf("no such file: %s\n", argv[3]); return 1; }
        b = (u8 *)malloc((size_t)size + 1u);
        if (ps2_vfs_read_path(argv[3], 0, (u32)size, b) != size) return 1;
        fp = fopen(argv[4], "wb");
        if (!fp || fwrite(b, 1, (size_t)size, fp) != (size_t)size) return 1;
        fclose(fp);
        printf("cat: %s -> %s (%lld bytes)\n", argv[3], argv[4], (long long)size);
        return 0;
    }
    return 2;
}
