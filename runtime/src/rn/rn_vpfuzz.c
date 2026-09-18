#include "rn_vp_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u64 rs;
static u32 rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (u32)(rs >> 16);
}
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFFu) / 16777216.0f; }
static int chance(u32 pct) { return rnd() % 100u < pct; }

static ps2_vu *V;

static void qf(u32 a, float x, float y, float z, float w) {
    float f[4] = { x, y, z, w };
    memcpy(V->mem + (a & VP_QW_MASK) * 16u, f, 16);
}
static void qi(u32 a, u32 x, u32 y, u32 z, u32 w) {
    u32 v[4] = { x, y, z, w };
    memcpy(V->mem + (a & VP_QW_MASK) * 16u, v, 16);
}
static void qcopy(u32 dst, const float *f) { memcpy(V->mem + (dst & VP_QW_MASK) * 16u, f, 16); }

static void qtag(u32 a, u32 nloop, int eop, int pre, u32 prim, u32 flg, u32 nreg, u64 regs) {
    u64 lo = (u64)(nloop & 0x7FFFu) | ((u64)(eop ? 1 : 0) << 15) | ((u64)(pre ? 1 : 0) << 46)
           | ((u64)(prim & 0x7FFu) << 47) | ((u64)(flg & 3u) << 58) | ((u64)(nreg & 15u) << 60);
    u32 v[4] = { (u32)lo, (u32)(lo >> 32), (u32)regs, (u32)(regs >> 32) };
    memcpy(V->mem + (a & VP_QW_MASK) * 16u, v, 16);
}

typedef struct { float S[4][4], C[4][4], B[4][4]; } cam;

static void make_cam(cam *c) {
    float fx = frand(200.0f, 600.0f), fy = fx * frand(0.8f, 1.1f);
    float zs = frand(0.0f, 2.0f), zb = frand(1.0e6f, 8.0e6f), nearz = frand(0.5f, 20.0f);
    memset(c, 0, sizeof *c);
    c->S[0][0] = fx;
    c->S[1][1] = fy;
    c->S[2][0] = 2048.0f; c->S[2][1] = 2048.0f; c->S[2][2] = zs; c->S[2][3] = 1.0f;
    c->S[3][2] = zb;
    c->C[0][0] = fx / 256.0f;
    c->C[1][1] = fy / 224.0f;
    c->C[2][2] = 1.0f; c->C[2][3] = 1.0f;
    c->C[3][2] = -2.0f * nearz;
    {   float a = frand(-3.14f, 3.14f), s = sinf(a), k = cosf(a);
        c->B[0][0] = k;  c->B[0][2] = -s;
        c->B[1][1] = 1.0f;
        c->B[2][0] = s;  c->B[2][2] = k;
        c->B[3][0] = frand(-50.0f, 50.0f); c->B[3][1] = frand(-20.0f, 20.0f);
        c->B[3][2] = frand(-30.0f, 300.0f); c->B[3][3] = 1.0f;
    }
}

static void put_rows(u32 a, float r[4][4]) { for (int k = 0; k < 4; k++) qcopy(a + (u32)k, r[k]); }

static void rand_point(float *p, float spread) {
    p[0] = frand(-spread, spread);
    p[1] = frand(-spread * 0.6f, spread * 0.6f);
    p[2] = chance(85) ? frand(5.0f, spread * 4.0f) : frand(-spread, 5.0f);
    p[3] = 1.0f;
}

static void rand_regs(void) {
    for (int r = 1; r < 32; r++)
        for (int i = 0; i < 4; i++) V->vf[r].f[i] = frand(-4.0f, 4.0f);
    for (int r = 1; r < 16; r++) V->vi[r] = (u16)rnd();
}

typedef struct {
    u32 start;
    const char *name;
    void (*init)(void);
    u32 init_pc;
    u32 (*batch)(void);
    u32 batches;
} fuzz_gen;

static void load_init(void) {
    cam c;
    make_cam(&c);
    put_rows(0x10u, c.S);
    put_rows(0x14u, c.C);
    qf(0x06u, frand(1, 64), 0, 0, 0);
    qf(0x07u, 0, 0, frand(1, 64), 0);
    qf(0x08u, frand(1, 64), 0, frand(1, 64), 0);
    qtag(0x02u, 1, 0, 1, 0x4Cu, 0, 5, 0x44414ull);
    qtag(0x03u, 1, 0, 1, 0x4Cu, 0, 6, 0x441441ull);
    qtag(0x04u, 1, 0, 1, 0x42u, 0, 6, 0x444441ull);
    qtag(0x05u, 1, 0, 1, 0x41u, 0, 5, 0x44441ull);
    qtag(0x0Au, 0, 1, 0, 0, 0, 0, 0);
}
static u32 load_batch(void) {
    u32 top = V->top = chance(50) ? 0x40u : 0x220u;
    static const u16 modes[] = { 0, 1, 2, 3, 7, 0x80, 0x81, 0x82, 0x83 };
    u16 mode = modes[rnd() % (sizeof modes / sizeof modes[0])];
    float p[4];
    rand_point(p, 60.0f);
    qf(top, p[0], 0, p[2], 0);
    {   u32 w; memcpy(&w, V->mem + top * 16u + 4u, 4); w = (w & 0xFFFF0000u) | mode;
        memcpy(V->mem + top * 16u + 4u, &w, 4); }
    qf(top + 1u, 0, frand(-10, 10), 0, 0);
    qf(top + 2u, 0, frand(-10, 10), 0, 0);
    qf(top + 3u, 0, frand(-10, 10), 0, 0);
    for (u32 k = 0; k < 5; k++) qi(top + 4u + k, rnd() & 255u, rnd() & 255u, rnd() & 255u, 128);
    return 0x68u;
}

static void trees_init(void) {
    cam c;
    make_cam(&c);
    put_rows(0x10u, c.S);
    put_rows(0x14u, c.C);
    qf(0x18u, 0, 0, frand(0.5f, 2.0f), frand(-1000.0f, 1000.0f));
    {   float nearv = frand(50.0f, 200.0f), farv = nearv + frand(100.0f, 800.0f);
        qf(0x19u, farv, nearv, frand(nearv, farv), frand(nearv, farv)); }
    qf(0x1Au, frand(-5, 5), frand(-5, 5), frand(-5, 5), 1);
    qtag(0x02u, 2, 0, 0, 0, 0, 1, 0xEull);
    qi(0x2Du, 0x7000Du, 0, 0x47u, 0);
    qi(0x2Eu, 0x7014Du, 0, 0x47u, 0);
    qtag(0x06u, 2, 1, 1, 0x36u, 0, 3, 0x421ull);
    qtag(0x05u, 2, 1, 1, 0x76u, 0, 3, 0x421ull);
    qtag(0x01u, 2, 0, 1, 0x36u, 0, 3, 0x421ull);
    qtag(0x00u, 2, 0, 1, 0x76u, 0, 3, 0x421ull);
    qtag(0x04u, 0, 1, 0, 0, 0, 0, 0);
    for (u32 t = 0; t < 3; t++) {
        qi(0x1Bu + t, rnd(), rnd(), 0x06u, 0);
        qi(0x1Eu + t, rnd(), rnd(), 0x06u, 0);
        qi(0x21u + t, rnd(), rnd(), 0x06u, 0);
        qf(0x24u + t, frand(-8, 8), frand(-16, 0), 0, 0);
        qf(0x27u + t, frand(0, 0.5f), frand(0.5f, 1), 0, 0);
        qf(0x2Au + t, frand(2, 12), frand(4, 24), 0, 0);
    }
    qf(0x32u, frand(0, 255), frand(-1, 1), 0, 0);
}
static u32 trees_batch(void) {
    u32 top = V->top = chance(50) ? 0x40u : 0x220u;
    u16 mode = (u16)(rnd() % 3u);
    u32 a = top + 1u;
    float base[4];
    rand_point(base, 40.0f);
    if (mode != 1u) {
        u16 n = (u16)(1u + rnd() % 40u);
        qi(top, n, mode, 0x30u + n, rnd() % 3u);
        qf(a++, base[0], base[1], base[2], 0);
        for (u16 k = 0; k < n; k++) {
            float p[4];
            rand_point(p, 60.0f);
            qf(a++, p[0], 0, p[2], 1);
        }
    } else {
        u16 groups = (u16)(1u + rnd() % 3u);
        qi(top, groups, 1, 0x80u, 0);
        for (u16 g = 0; g < groups; g++) {
            u16 e0 = (u16)(rnd() % 5u), e1 = (u16)(e0 + rnd() % 5u), e2 = (u16)(e1 + rnd() % 5u);
            qf(a++, base[0], base[1], base[2], 0);
            qi(a++, e0, e1, e2, 0);
            for (u16 k = 0; k < e2; k++) {
                float p[4];
                rand_point(p, 60.0f);
                qf(a++, p[0], 0, p[2], 1);
            }
        }
    }
    return 0x68u;
}

static void rib_init(void) {
    cam c;
    make_cam(&c);
    put_rows(0x10u, c.S);
    put_rows(0x14u, c.C);
    qi(0x18u, chance(50) ? 0x320u : 0x384u, 0, 0, 0);
    qf(0x1Au, frand(0, 128), frand(-0.5f, 0.5f), 0, 0);
    qtag(0x1Cu, 2, 0, 0, 0, 0, 1, 0xEull);
    qtag(0x1Du, 0, 0, 1, 0x5Cu, 0, 3, 0x512ull);
    qtag(0x1Fu, 0, 1, 0, 0, 0, 0, 0);
}
static u32 rib_batch(void) {
    u32 top = V->top = chance(50) ? 0x100u : 0x1A0u;
    float c0[4], k[8];
    static int inside = -1;
    if (inside < 0) { const char *e = getenv("PS2_VPFUZZ_INSIDE"); inside = e && *e == '1'; }
    rand_point(c0, 40.0f);
    if (inside) { c0[0] = frand(-2, 2); c0[1] = frand(-2, 2); c0[2] = frand(100, 200); }
    qi(top + 0u, rnd(), rnd(), 0x06u, 0);
    qi(top + 1u, rnd(), rnd(), 0x42u, 0);
    qf(top + 2u, frand(-0.3f, 1.1f), frand(0.2f, 8.0f), frand(0, 200), frand(0, 100));
    qi(top + 5u, rnd() & 255u, rnd() & 255u, rnd() & 255u, rnd() & 255u);
    qf(top + 6u, frand(0, 0.5f), frand(0, 0.5f), frand(0.5f, 1), frand(0.5f, 1));
    for (u32 s = 0; s < 8; s++) {
        float d = inside ? 2.0f : chance(20) ? 60.0f : 6.0f;
        qf(top + 7u + s, c0[0] + frand(-d, d), c0[1] + frand(-d, d), c0[2] + frand(-d, d), frand(-2, 2));
        float h = inside ? 0.3f : 3.0f;
        qf(top + 0x0Fu + s, frand(-h, h), frand(-h, h), frand(-h, h), frand(-2, 2));
    }
    k[0] = frand(0, 40);
    for (int i = 1; i < 8; i++) k[i] = k[i - 1] + (chance(10) ? 0.0f : frand(1, 40));
    qf(top + 0x17u, k[0], k[1], k[2], k[3]);
    qf(top + 0x18u, k[4], k[5], k[6], k[7]);
    qf(top + 0x19u, frand(0, 255), frand(0, 255), frand(0, 255), frand(0, 255));
    qf(top + 0x1Au, frand(0, 255), frand(0, 255), frand(0, 255), frand(0, 255));
    for (u32 q = 0x1Bu; q < 0x27u; q++)
        qf(top + q, c0[0] + frand(-5, 5), c0[1] + frand(-5, 5), c0[2] + frand(-5, 5), 1);
    return 0x50u;
}

static void pt_curve(u32 a) {
    float k[8];
    k[0] = frand(0, 40);
    for (int i = 1; i < 8; i++) k[i] = k[i - 1] + (chance(10) ? 0.0f : frand(1, 40));
    qf(a + 0u, k[0], k[1], k[2], k[3]);
    qf(a + 1u, k[4], k[5], k[6], k[7]);
    qf(a + 2u, frand(0, 255), frand(0, 255), frand(0, 255), frand(0, 255));
    qf(a + 3u, frand(0, 255), frand(0, 255), frand(0, 255), frand(0, 255));
}
static void pt_init(void) {
    cam c;
    float px = frand(0.5f, 2.0f), py = frand(0.5f, 2.0f);
    make_cam(&c);
    put_rows(0x10u, c.S);
    put_rows(0x14u, c.C);
    qi(0x18u, chance(50) ? 0x190u : 0x2BCu, 0, 0, 0);
    qf(0x1Au, frand(0.5f, 2.0f), frand(-0.005f, 0.005f), 0, 0);
    qf(0x1Bu, px, py, 1.0f / px, 1.0f / py);
    qtag(0x1Cu, 2, 0, 0, 0, 0, 1, 0xEull);
    qtag(0x1Du, 4, 0, 1, 0x5Cu, 0, 3, 0x513ull);
    qtag(0x1Eu, 2, 0, 1, 0x49u, 0, 2, 0x51ull);
    qtag(0x1Fu, 0, 1, 0, 0, 0, 0, 0);
    for (u32 i = 0; i < 128; i++) qf(0x21u + i, frand(0, 1), frand(0, 1), frand(0, 1), frand(0, 1));
    for (u32 i = 0; i < 32; i++) qf(0xA2u + i, (float)(rnd() & 1u), (float)(rnd() & 1u), 0, 0);
}
static u32 pt_batch(void) {
    u32 top = V->top = chance(50) ? 0xC8u : 0x12Cu;
    u16 n = (u16)(1u + rnd() % 24u);
    float p[4], step[3];
    static int inside = -1;
    if (inside < 0) { const char *e = getenv("PS2_VPFUZZ_INSIDE"); inside = e && *e == '1'; }
    rand_point(p, 40.0f);
    if (inside) { p[0] = frand(-5, 5); p[1] = frand(-5, 5); p[2] = frand(30, 200); }
    {   float len = chance(30) ? frand(5, 40) : frand(0.01f, 3);
        for (int i = 0; i < 3; i++) step[i] = frand(-len, len);
        if (inside) for (int i = 0; i < 3; i++) step[i] *= 0.1f; }
    qi(top + 0u, rnd(), rnd(), 0x06u, 0);
    qi(top + 1u, rnd(), rnd(), 0x42u, 0);
    qi(top + 2u, n, 0, 0, 0);
    qf(top + 3u, frand(0, 3), frand(0, 400), frand(0, 2), frand(0, 1000));
    qf(top + 4u, frand(0, 0.3f), frand(0, 0.1f), frand(0.5f, 2), frand(-0.2f, 0.5f));
    qf(top + 5u, frand(0, 100), frand(0, 30), frand(0, 0.1f), 0);
    qi(top + 6u, rnd() & 255u, rnd() & 255u, rnd() & 255u, rnd() & 255u);
    for (u32 k = 0; k < 4; k++) qi(top + 7u + k, rnd() & 255u, rnd() & 255u, rnd() & 255u, rnd() & 255u);
    qf(top + 0x0Bu, frand(0, 256), frand(0, 256), frand(16, 256), frand(0, 64));
    pt_curve(top + 0x0Cu);
    pt_curve(top + 0x10u);
    for (u16 i = 0; i < n; i++) {
        qf(top + 0x34u + 2u * i, p[0] + step[0] * i + frand(-0.5f, 0.5f),
           p[1] + step[1] * i + frand(-0.5f, 0.5f), p[2] + step[2] * i + frand(-0.5f, 0.5f), 1);
        qf(top + 0x35u + 2u * i, frand(-2, 2), frand(-2, 2), frand(-2, 2), (float)(rnd() % 1000u));
    }
    return 0x50u;
}

static void rain_init(void) {
    float px = frand(0.5f, 2.0f), py = frand(0.5f, 2.0f);
    qf(0x1Bu, px, py, 1.0f / px, 1.0f / py);
    qtag(0x1Cu, 2, 0, 0, 0, 0, 1, 0xEull);
    qtag(0x1Du, 4, 0, 1, 0x5Cu, 0, 3, 0x513ull);
    qtag(0x1Eu, 2, 0, 1, 0x49u, 0, 2, 0x51ull);
    qtag(0x1Fu, 1, 0, 1, 0x56u, 0, 5, 0x53531ull);
    qtag(0x20u, 0, 1, 0, 0, 0, 0, 0);
    for (u32 i = 0; i < 32; i++) qf(0xA2u + i, (float)(rnd() & 1u), (float)(rnd() & 1u), 0, 0);
}
static void rain_rows(u32 a, cam *c, float m) {
    float r[4][4];
    memcpy(r, c->S, sizeof r);
    r[3][0] += frand(-m, m); r[3][1] += frand(-m, m);
    put_rows(a, r);
}
static u32 rain_batch(void) {
    u32 top = V->top = chance(50) ? 0xC8u : 0x190u;
    u32 total = 1u + rnd() % 85u, na = rnd() % (total + 1u), nb = rnd() % (total + 1u - na);
    cam c;
    make_cam(&c);
    qi(top + 0u, rnd(), rnd(), 0x42u, 0);
    qi(top + 1u, rnd(), rnd(), 0x06u, 0);
    put_rows(top + 2u, c.S);
    rain_rows(top + 6u, &c, 3000.0f);
    rain_rows(top + 0x0Au, &c, 3000.0f);
    rain_rows(top + 0x0Eu, &c, 3000.0f);
    qf(top + 0x12u, (float)na, (float)(na + nb), (float)(total - na - nb), (float)(total - 1u));
    qf(top + 0x13u, frand(50, 300), frand(-50, 50), 0, 0);
    qf(top + 0x14u, (2048 - 320) * 16.0f, (2048 + 320) * 16.0f, (2048 - 224) * 16.0f,
       (2048 + 224) * 16.0f);
    qf(top + 0x15u, frand(100, 3000), chance(50) ? frand(100, 3000) : frand(3000, 300000),
       frand(0.5f, 20), frand(0, 10));
    qi(top + 0x16u, rnd() & 255u, rnd() & 255u, rnd() & 255u, rnd() & 255u);
    qf(top + 0x17u, frand(0, 256), frand(0, 256), frand(16, 256), frand(0, 64));
    qf(top + 0x18u, frand(0, 1024), frand(0, 256), frand(16, 512), frand(0, 1024));
    for (u32 k = 0; k < 3; k++)
        qf(top + 0x19u + k, (float)(rnd() & 4095u), (float)(rnd() & 4095u), (float)(rnd() & 4095u), 0);
    for (u32 k = 0; k < 85; k++)
        qf(top + 0x1Cu + k, frand(0, 1), frand(0, 1), frand(0, 1), frand(0, 1));
    return 0x10u;
}

static void flat_rows(void) {
    cam c;
    make_cam(&c);
    put_rows(0x10u, c.S);
    put_rows(0x14u, c.C);
    put_rows(0x28u, c.B);
    qi(0x1Bu, chance(85) ? 0x20u : 0u, 0, 1u + rnd() % 3u, 0);
}
static void flat_init(void) {
    qtag(0x00u, 0, 0, 1, 0x04u, 0, 1, 0x5ull);
    qtag(0x01u, 0, 0, 1, 0x05u, 0, 1, 0x5ull);
    qtag(0x02u, 0, 1, 0, 0, 0, 0, 0);
    for (u32 k = 0; k < 4; k++) qf(0x30u + k, frand(-2, 2), frand(-2, 2), frand(-2, 2), frand(-2, 2));
    flat_rows();
}
static u32 flat_batch(void) {
    u32 top = V->top = chance(50) ? 0x40u : 0x1D8u;
    u16 n = (u16)(3u + rnd() % 38u);
    float base[4], spread = chance(50) ? 4.0f : 40.0f;
    if (chance(25)) flat_rows();
    rand_point(base, 40.0f);
    qi(top, n, 0, 0, 0);
    for (u16 k = 0; k < n; k++) {
        u32 a = top + 1u + 5u * k;
        qf(a, base[0] + frand(-spread, spread), base[1] + frand(-spread, spread),
           base[2] + frand(-spread, spread), 1);
        qf(a + 1u, frand(-1, 1), frand(-1, 1), frand(-1, 1), frand(-1, 1));
        qf(a + 2u, frand(-1, 1), frand(-1, 1), frand(-1, 1), frand(-1, 1));
        qf(a + 3u, frand(0, 1), frand(0, 1), frand(0, 1), 0);
        qf(a + 4u, frand(-1, 1), frand(-1, 1), frand(-1, 1), frand(-1, 1));
    }
    return 0x10u;
}

static const fuzz_gen gens[] = {
    { 0x003C3D80u, "ribbons", rib_init, 0, rib_batch, 4 },
    { 0x003C06F0u, "particles", pt_init, 0, pt_batch, 4 },
    { 0x003C2240u, "rain", rain_init, 0, rain_batch, 2 },
    { 0x003A9350u, "flat", flat_init, 0, flat_batch, 6 },
    { 0x003BE3E0u, "loadmap", load_init, 0, load_batch, 8 },
    { 0x003C5220u, "trees", trees_init, 0, trees_batch, 8 },
};

int rn_vpfuzz_main(u32 start, u32 iterations, u32 seed) {
    const fuzz_gen *g = NULL;
    u64 st[7];
    int prog;
    for (u32 i = 0; i < sizeof gens / sizeof gens[0]; i++)
        if (gens[i].start == start) g = &gens[i];
    if (!g) {
        ps2_log("vp-fuzz: no generator for the program at %08X", start);
        return 2;
    }
    V = ps2_vu1_test_vu();
    if (!V || !V->mem || !V->micro) { ps2_log("vp-fuzz: no VU1"); return 2; }
    prog = rn_vp_upload(V, start);
    if (prog < 0) { ps2_log("vp-fuzz: %08X could not be put in micro memory", start); return 2; }
    rs = 0x9E3779B97F4A7C15ull ^ ((u64)seed << 17) ^ seed;
    rn_vp_mode = 2;
    {   const char *e = getenv("PS2_VPFUZZ_FROM");
        if (e) { u32 skip = (u32)strtoul(e, NULL, 0);
                 for (u32 it = 0; it < skip; it++) {
                     memset(V->mem, 0, V->mem_size); rand_regs(); g->init();
                     for (u32 b = 0; b < g->batches; b++) (void)g->batch();
                 }
                 ps2_log("vp-fuzz: starting at round %u", skip); } }
    for (u32 it = 0; it < iterations; it++) {
        u64 before[7];
        rn_vp_verify_stats(prog, before);
        memset(V->mem, 0, V->mem_size);
        rand_regs();
        V->vf[0].f[0] = V->vf[0].f[1] = V->vf[0].f[2] = 0.0f; V->vf[0].f[3] = 1.0f;
        V->vi[0] = 0;
        g->init();
        ps2_vu1_test_run(g->init_pc);
        for (u32 b = 0; b < g->batches; b++) {
            u32 pc = g->batch();
            ps2_vu1_test_run(pc);
        }
        {   u64 after[7];
            static int shown;
            rn_vp_verify_stats(prog, after);
            if ((after[2] != before[2] || after[3] != before[3]) && shown++ < 4)
                ps2_log("vp-fuzz: round %u has a difference", it);
        }
    }
    rn_vp_verify_stats(prog, st);
    ps2_log("vp-fuzz: %s (%08X), %u rounds: %llu kicks compared, %llu identical, %llu "
            "differ, %llu kicked elsewhere, %llu never kicked; %llu activations native, "
            "%llu declined", g->name, start, iterations, (unsigned long long)st[0],
            (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3],
            (unsigned long long)st[4], (unsigned long long)st[5], (unsigned long long)st[6]);
    rn_vp_report();
    return (st[2] || st[3] || st[4]) ? 1 : 0;
}
