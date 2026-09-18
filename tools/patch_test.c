#include "ps2_runtime.h"
#include "ps2_patch.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8 *ps2_pt[PS2_PT_ENTRIES];
u8 *ps2_ram;
static int log_lines;
static char last_log[1024];

void ps2_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", last_log);
    log_lines++;
}
void ps2_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void ps2_text_bounds(u32 *lo, u32 *hi) { *lo = 0x00100000u; *hi = 0x00390000u; }
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

static u32 rd32(u32 a) { u32 v; memcpy(&v, ps2_ram + a, 4); return v; }
static void wr32(u32 a, u32 v) { memcpy(ps2_ram + a, &v, 4); }

int main(int argc, char **argv) {
    const char *pnach = argc > 1 ? argv[1] : "patch_test.pnach";
    int h, added;
    u8 b4[4], o4[4];
    ps2_ram = (u8 *)calloc(1, PS2_RAM_SIZE);
    for (u32 p = 0; p < PS2_RAM_SIZE >> PS2_PAGE_BITS; p++) {
        ps2_pt[p] = ps2_ram + ((size_t)p << PS2_PAGE_BITS);
        ps2_pt[(0x20000000u >> PS2_PAGE_BITS) + p] = ps2_ram + ((size_t)p << PS2_PAGE_BITS);
    }
    wr32(0x00440828u, 0x440315C2u);
    wr32(0x0044082Cu, 0x441013D7u);

    {
        FILE *fp = fopen(pnach, "w");
        fputs("// a comment line\n"
              "gametitle=Ace Combat 5 (SLUS-20851)\n"
              "[Widescreen 16:9]\n"
              "gsaspectratio=16:9\n"
              "author=nemesis2000\n"
              "patch=1,EE,00440828,word,43D638F3   // trailing comment\n"
              "patch=1,EE,0044082C,word,43EB7385\n"
              "[Code patch]\n"
              "patch=1,EE,00200000,word,00000000\n"
              "patch=1,IOP,00001000,word,00000000\n"
              "[Extended]\n"
              "patch=0,EE,20450000,extended,12345678\n"
              "patch=0,EE,00450010,extended,000000AB\n"
              "patch=0,EE,D0450000,extended,00000001\n"
              "patch=0,EE,20450020,extended,DEADBEEF\n"
              "[Other types]\n"
              "patch=0,EE,00450030,bytes,0102030405\n"
              "patch=0,EE,00450040,beword,11223344\n"
              "patch=2,EE,00450050,short,BEEF\n"
              "patch=0,EE,00450060,double,0102030405060708\n"
              "patch=0,EE,nothex,word,1\n"
              "patch=0,EE,00450070\n", fp);
        fclose(fp);
    }
    added = ps2_patch_load_pnach(pnach, 0, "pnach_mod");
    CHECK(added == 8, "8 patches from the file, got %d", added);
    ps2_patch_start();
    CHECK(rd32(0x00440828u) == 0x440315C2u,
          "place 1 is not a start-up patch: nothing until the first field");
    ps2_patch_field();
    CHECK(rd32(0x00440828u) == 0x43D638F3u && rd32(0x0044082Cu) == 0x43EB7385u,
          "widescreen words written on the first field");
    CHECK(rd32(0x00450000u) == 0x12345678u, "extended 32-bit");
    CHECK(ps2_ram[0x00450010u] == 0xAB, "extended 8-bit");
    CHECK(rd32(0x00450020u) == 0, "after a conditional code, the rest of the section is skipped");
    CHECK(!memcmp(ps2_ram + 0x00450030u, "\x01\x02\x03\x04\x05", 5), "bytes");
    CHECK(rd32(0x00450040u) == 0x44332211u, "beword is big-endian in memory");
    CHECK(ps2_ram[0x00450050u] == 0xEF && ps2_ram[0x00450051u] == 0xBE, "short");
    CHECK(ps2_ram[0x00450060u] == 0x08 && ps2_ram[0x00450067u] == 0x01, "double");
    CHECK(rd32(0x00200000u) == 0, "a code patch is refused");

    wr32(0x00440828u, 0);
    wr32(0x00450000u, 0);
    ps2_patch_field();
    CHECK(rd32(0x00440828u) == 0x43D638F3u, "place 1 re-applied each field");
    CHECK(rd32(0x00450000u) == 0, "place 0 applied once only");
    CHECK(ps2_ram[0x00450050u] == 0xEF, "place 2 applied each field");

    memset(b4, 0x11, 4);
    h = ps2_patch_add(0x00460000u, b4, NULL, 4, PS2_PATCH_ALWAYS, 0, "low", "t:1");
    CHECK(h > 0, "low added");
    memset(b4, 0x22, 2);
    h = ps2_patch_add(0x00460002u, b4, NULL, 2, PS2_PATCH_ALWAYS, 5, "high", "t:2");
    CHECK(h > 0, "high takes the overlap");
    memset(b4, 0x33, 4);
    CHECK(ps2_patch_add(0x00460000u, b4, NULL, 4, PS2_PATCH_ALWAYS, 1, "mid", "t:3") < 0,
          "mid is refused: high holds overlapping bytes");
    ps2_patch_field();
    CHECK(ps2_ram[0x00460000u] == 0x11 || ps2_ram[0x00460000u] == 0x00,
          "low is off after losing");
    CHECK(ps2_ram[0x00460002u] == 0x22, "high applies");
    memset(ps2_ram + 0x00460000u, 0, 4);
    ps2_patch_field();
    CHECK(ps2_ram[0x00460000u] == 0x00, "low no longer applies");

    wr32(0x00470000u, 0xAAAAAAAAu);
    memcpy(b4, "\x01\x00\x00\x00", 4);
    memcpy(o4, "\xBB\xBB\xBB\xBB", 4);
    h = ps2_patch_add(0x00470000u, b4, o4, 4, PS2_PATCH_ONCE, 0, "orig", "t:4");
    CHECK(h > 0 && rd32(0x00470000u) == 0xAAAAAAAAu,
          "a patch that finds the wrong original is not written");
    memcpy(o4, "\xAA\xAA\xAA\xAA", 4);
    wr32(0x00470010u, 0xAAAAAAAAu);
    h = ps2_patch_add(0x00470010u, b4, o4, 4, PS2_PATCH_ONCE, 0, "orig", "t:5");
    CHECK(h > 0 && rd32(0x00470010u) == 1u, "matching original: written");
    CHECK(ps2_patch_remove(h) == 0 && rd32(0x00470010u) == 0xAAAAAAAAu,
          "removal puts the original back");
    CHECK(ps2_patch_remove(h) < 0, "a removed patch cannot be removed twice");

    CHECK(ps2_patch_add(0x20150000u, b4, NULL, 4, PS2_PATCH_ONCE, 0, "x", "t:6") < 0,
          "code through a mirror is refused");
    CHECK(ps2_patch_add(0x12000000u, b4, NULL, 4, PS2_PATCH_ONCE, 0, "x", "t:7") < 0,
          "hardware registers are refused");

    ps2_patch_report();
    printf("%s\n", fails ? "patch: FAILED" : "patch: all checks passed");
    return fails != 0;
}
