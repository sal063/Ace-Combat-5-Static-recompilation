#ifndef PS2_MMI_H
#define PS2_MMI_H

#ifndef PS2_RUNTIME_H
#error "include ps2_runtime.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MMI_RD ps2_reg128 *d = &ctx->r[rd]
#define MMI_GUARD if (rd == 0) return

PS2_INLINE s32 ps2_sat_s32(s64 v) {
    if (v > 0x7FFFFFFFll) return (s32)0x7FFFFFFF;
    if (v < -0x80000000ll) return (s32)0x80000000;
    return (s32)v;
}
PS2_INLINE s16 ps2_sat_s16(s32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (s16)v;
}
PS2_INLINE s8 ps2_sat_s8(s32 v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (s8)v;
}
PS2_INLINE u32 ps2_sat_u32(s64 v) { return v > 0xFFFFFFFFll ? 0xFFFFFFFFu
                                         : (v < 0 ? 0u : (u32)v); }
PS2_INLINE u16 ps2_sat_u16(s32 v) { return v > 0xFFFF ? 0xFFFFu
                                         : (v < 0 ? 0u : (u16)v); }
PS2_INLINE u8 ps2_sat_u8(s32 v) { return v > 0xFF ? 0xFFu
                                       : (v < 0 ? 0u : (u8)v); }

#define MMI_W_OP(name, expr)                                                   \
    PS2_INLINE void ps2_##name(ps2_ctx *ctx, int rd, int rs, int rt) {         \
        MMI_GUARD;                                                             \
        ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;                          \
        for (int i = 0; i < 4; i++) { expr; }                                  \
        ctx->r[rd] = o;                                                        \
    }
MMI_W_OP(paddw,  o.sw[i] = a.sw[i] + b.sw[i])
MMI_W_OP(psubw,  o.sw[i] = a.sw[i] - b.sw[i])
MMI_W_OP(pcgtw,  o.uw[i] = a.sw[i] > b.sw[i] ? 0xFFFFFFFFu : 0u)
MMI_W_OP(pceqw,  o.uw[i] = a.sw[i] == b.sw[i] ? 0xFFFFFFFFu : 0u)
MMI_W_OP(pmaxw,  o.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
MMI_W_OP(pminw,  o.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
MMI_W_OP(paddsw, o.sw[i] = ps2_sat_s32((s64)a.sw[i] + (s64)b.sw[i]))
MMI_W_OP(psubsw, o.sw[i] = ps2_sat_s32((s64)a.sw[i] - (s64)b.sw[i]))
MMI_W_OP(padduw, o.uw[i] = ps2_sat_u32((s64)(u64)a.uw[i] + (s64)(u64)b.uw[i]))
MMI_W_OP(psubuw, o.uw[i] = ps2_sat_u32((s64)(u64)a.uw[i] - (s64)(u64)b.uw[i]))
#undef MMI_W_OP

PS2_INLINE void ps2_pabsw(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++)
        o.sw[i] = b.sw[i] == (s32)0x80000000 ? (s32)0x7FFFFFFF
                                             : (b.sw[i] < 0 ? -b.sw[i] : b.sw[i]);
    ctx->r[rd] = o;
}

#define MMI_H_OP(name, expr)                                                   \
    PS2_INLINE void ps2_##name(ps2_ctx *ctx, int rd, int rs, int rt) {         \
        MMI_GUARD;                                                             \
        ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;                          \
        for (int i = 0; i < 8; i++) { expr; }                                  \
        ctx->r[rd] = o;                                                        \
    }
MMI_H_OP(paddh,  o.sh[i] = (s16)(a.sh[i] + b.sh[i]))
MMI_H_OP(psubh,  o.sh[i] = (s16)(a.sh[i] - b.sh[i]))
MMI_H_OP(pcgth,  o.uh[i] = a.sh[i] > b.sh[i] ? 0xFFFFu : 0u)
MMI_H_OP(pceqh,  o.uh[i] = a.sh[i] == b.sh[i] ? 0xFFFFu : 0u)
MMI_H_OP(pmaxh,  o.sh[i] = a.sh[i] > b.sh[i] ? a.sh[i] : b.sh[i])
MMI_H_OP(pminh,  o.sh[i] = a.sh[i] < b.sh[i] ? a.sh[i] : b.sh[i])
MMI_H_OP(paddsh, o.sh[i] = ps2_sat_s16((s32)a.sh[i] + (s32)b.sh[i]))
MMI_H_OP(psubsh, o.sh[i] = ps2_sat_s16((s32)a.sh[i] - (s32)b.sh[i]))
MMI_H_OP(padduh, o.uh[i] = ps2_sat_u16((s32)a.uh[i] + (s32)b.uh[i]))
MMI_H_OP(psubuh, o.uh[i] = ps2_sat_u16((s32)a.uh[i] - (s32)b.uh[i]))
#undef MMI_H_OP

PS2_INLINE void ps2_pabsh(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++)
        o.sh[i] = b.sh[i] == (s16)0x8000 ? (s16)0x7FFF
                                         : (b.sh[i] < 0 ? (s16)-b.sh[i] : b.sh[i]);
    ctx->r[rd] = o;
}

PS2_INLINE void ps2_padsbh(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.sh[i] = (s16)(a.sh[i] - b.sh[i]);
    for (int i = 4; i < 8; i++) o.sh[i] = (s16)(a.sh[i] + b.sh[i]);
    ctx->r[rd] = o;
}

#define MMI_B_OP(name, expr)                                                   \
    PS2_INLINE void ps2_##name(ps2_ctx *ctx, int rd, int rs, int rt) {         \
        MMI_GUARD;                                                             \
        ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;                          \
        for (int i = 0; i < 16; i++) { expr; }                                 \
        ctx->r[rd] = o;                                                        \
    }
MMI_B_OP(paddb,  o.sb[i] = (s8)(a.sb[i] + b.sb[i]))
MMI_B_OP(psubb,  o.sb[i] = (s8)(a.sb[i] - b.sb[i]))
MMI_B_OP(pcgtb,  o.ub[i] = a.sb[i] > b.sb[i] ? 0xFFu : 0u)
MMI_B_OP(pceqb,  o.ub[i] = a.sb[i] == b.sb[i] ? 0xFFu : 0u)
MMI_B_OP(paddsb, o.sb[i] = ps2_sat_s8((s32)a.sb[i] + (s32)b.sb[i]))
MMI_B_OP(psubsb, o.sb[i] = ps2_sat_s8((s32)a.sb[i] - (s32)b.sb[i]))
MMI_B_OP(paddub, o.ub[i] = ps2_sat_u8((s32)a.ub[i] + (s32)b.ub[i]))
MMI_B_OP(psubub, o.ub[i] = ps2_sat_u8((s32)a.ub[i] - (s32)b.ub[i]))
#undef MMI_B_OP

#define MMI_L_OP(name, expr)                                                   \
    PS2_INLINE void ps2_##name(ps2_ctx *ctx, int rd, int rs, int rt) {         \
        MMI_GUARD;                                                             \
        ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;                          \
        o.ud[0] = (expr(a.ud[0], b.ud[0]));                                     \
        o.ud[1] = (expr(a.ud[1], b.ud[1]));                                     \
        ctx->r[rd] = o;                                                        \
    }
#define PS2_AND_(x, y) ((x) & (y))
#define PS2_OR_(x, y)  ((x) | (y))
#define PS2_XOR_(x, y) ((x) ^ (y))
#define PS2_NOR_(x, y) (~((x) | (y)))
MMI_L_OP(pand, PS2_AND_)
MMI_L_OP(por,  PS2_OR_)
MMI_L_OP(pxor, PS2_XOR_)
MMI_L_OP(pnor, PS2_NOR_)
#undef MMI_L_OP

PS2_INLINE void ps2_pextlw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    o.uw[0] = b.uw[0]; o.uw[1] = a.uw[0];
    o.uw[2] = b.uw[1]; o.uw[3] = a.uw[1];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pextuw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    o.uw[0] = b.uw[2]; o.uw[1] = a.uw[2];
    o.uw[2] = b.uw[3]; o.uw[3] = a.uw[3];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pextlh(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) { o.uh[i * 2] = b.uh[i]; o.uh[i * 2 + 1] = a.uh[i]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pextuh(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) { o.uh[i * 2] = b.uh[i + 4]; o.uh[i * 2 + 1] = a.uh[i + 4]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pextlb(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) { o.ub[i * 2] = b.ub[i]; o.ub[i * 2 + 1] = a.ub[i]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pextub(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) { o.ub[i * 2] = b.ub[i + 8]; o.ub[i * 2 + 1] = a.ub[i + 8]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_ppacw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    o.uw[0] = b.uw[0]; o.uw[1] = b.uw[2];
    o.uw[2] = a.uw[0]; o.uw[3] = a.uw[2];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_ppach(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.uh[i] = b.uh[i * 2];
    for (int i = 0; i < 4; i++) o.uh[i + 4] = a.uh[i * 2];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_ppacb(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) o.ub[i] = b.ub[i * 2];
    for (int i = 0; i < 8; i++) o.ub[i + 8] = a.ub[i * 2];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pext5(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) {
        u32 h = b.uw[i] & 0xFFFFu;
        o.uw[i] = ((h & 0x001Fu) << 3) | ((h & 0x03E0u) << 6) |
                  ((h & 0x7C00u) << 9) | ((h & 0x8000u) << 16);
    }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_ppac5(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) {
        u32 w = b.uw[i];
        o.uw[i] = ((w >> 3) & 0x001Fu) | ((w >> 6) & 0x03E0u) |
                  ((w >> 9) & 0x7C00u) | ((w >> 16) & 0x8000u);
    }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pcpyh(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.uh[i] = b.uh[0];
    for (int i = 4; i < 8; i++) o.uh[i] = b.uh[4];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pcpyld(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u64 lo = ctx->r[rt].ud[0], hi = ctx->r[rs].ud[0];
    ctx->r[rd].ud[0] = lo; ctx->r[rd].ud[1] = hi;
}
PS2_INLINE void ps2_pcpyud(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u64 lo = ctx->r[rs].ud[1], hi = ctx->r[rt].ud[1];
    ctx->r[rd].ud[0] = lo; ctx->r[rd].ud[1] = hi;
}
PS2_INLINE void ps2_pinth(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) { o.uh[i * 2] = b.uh[i]; o.uh[i * 2 + 1] = a.uh[i + 4]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pinteh(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) { o.uh[i * 2] = b.uh[i * 2]; o.uh[i * 2 + 1] = a.uh[i * 2]; }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pexeh(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o = b;
    o.uh[0] = b.uh[2]; o.uh[2] = b.uh[0];
    o.uh[4] = b.uh[6]; o.uh[6] = b.uh[4];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pexch(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o = b;
    o.uh[1] = b.uh[2]; o.uh[2] = b.uh[1];
    o.uh[5] = b.uh[6]; o.uh[6] = b.uh[5];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pexew(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o = b;
    o.uw[0] = b.uw[2]; o.uw[2] = b.uw[0];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pexcw(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o = b;
    o.uw[1] = b.uw[2]; o.uw[2] = b.uw[1];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_prevh(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.uh[i] = b.uh[3 - i];
    for (int i = 0; i < 4; i++) o.uh[i + 4] = b.uh[7 - i];
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_prot3w(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rs;
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    o.uw[0] = b.uw[1]; o.uw[1] = b.uw[2]; o.uw[2] = b.uw[0]; o.uw[3] = b.uw[3];
    ctx->r[rd] = o;
}

PS2_INLINE void ps2_psllh(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) o.uh[i] = (u16)(b.uh[i] << (sa & 15));
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psrlh(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) o.uh[i] = (u16)(b.uh[i] >> (sa & 15));
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psrah(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 8; i++) o.sh[i] = (s16)(b.sh[i] >> (sa & 15));
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psllw(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.uw[i] = b.uw[i] << (sa & 31);
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psrlw(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.uw[i] = b.uw[i] >> (sa & 31);
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psraw(ps2_ctx *ctx, int rd, int rt, int sa) {
    MMI_GUARD;
    ps2_reg128 b = ctx->r[rt], o;
    for (int i = 0; i < 4; i++) o.sw[i] = b.sw[i] >> (sa & 31);
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_psllvw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u32 s0 = ctx->r[rs].uw[0] & 31u, s1 = ctx->r[rs].uw[2] & 31u;
    s32 v0 = (s32)(ctx->r[rt].uw[0] << s0), v1 = (s32)(ctx->r[rt].uw[2] << s1);
    ctx->r[rd].sd[0] = v0; ctx->r[rd].sd[1] = v1;
}
PS2_INLINE void ps2_psrlvw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u32 s0 = ctx->r[rs].uw[0] & 31u, s1 = ctx->r[rs].uw[2] & 31u;
    s32 v0 = (s32)(ctx->r[rt].uw[0] >> s0), v1 = (s32)(ctx->r[rt].uw[2] >> s1);
    ctx->r[rd].sd[0] = v0; ctx->r[rd].sd[1] = v1;
}
PS2_INLINE void ps2_psravw(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u32 s0 = ctx->r[rs].uw[0] & 31u, s1 = ctx->r[rs].uw[2] & 31u;
    s32 v0 = ctx->r[rt].sw[0] >> s0, v1 = ctx->r[rt].sw[2] >> s1;
    ctx->r[rd].sd[0] = v0; ctx->r[rd].sd[1] = v1;
}
PS2_INLINE void ps2_qfsrv(ps2_ctx *ctx, int rd, int rs, int rt) {
    MMI_GUARD;
    u32 sa = ctx->sa & 127u;
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt], o;
    if (sa == 0) { ctx->r[rd] = b; return; }
    if (sa < 64) {
        o.ud[0] = (b.ud[0] >> sa) | (b.ud[1] << (64 - sa));
        o.ud[1] = (b.ud[1] >> sa) | (a.ud[0] << (64 - sa));
    } else if (sa == 64) {
        o.ud[0] = b.ud[1];
        o.ud[1] = a.ud[0];
    } else {
        u32 s = sa - 64;
        o.ud[0] = (b.ud[1] >> s) | (a.ud[0] << (64 - s));
        o.ud[1] = (a.ud[0] >> s) | (a.ud[1] << (64 - s));
    }
    ctx->r[rd] = o;
}

PS2_INLINE void ps2_pmfhi(ps2_ctx *ctx, int rd) { MMI_GUARD; ctx->r[rd] = ctx->hi; }
PS2_INLINE void ps2_pmflo(ps2_ctx *ctx, int rd) { MMI_GUARD; ctx->r[rd] = ctx->lo; }
PS2_INLINE void ps2_pmthi(ps2_ctx *ctx, int rs) { ctx->hi = ctx->r[rs]; }
PS2_INLINE void ps2_pmtlo(ps2_ctx *ctx, int rs) { ctx->lo = ctx->r[rs]; }

PS2_INLINE void ps2_pmfhl(ps2_ctx *ctx, int rd, int sa) {
    MMI_GUARD;
    ps2_reg128 lo = ctx->lo, hi = ctx->hi, o;
    switch (sa & 7) {
    case 0:
        o.uw[0] = lo.uw[0]; o.uw[1] = hi.uw[0];
        o.uw[2] = lo.uw[2]; o.uw[3] = hi.uw[2];
        break;
    case 1:
        o.uw[0] = lo.uw[1]; o.uw[1] = hi.uw[1];
        o.uw[2] = lo.uw[3]; o.uw[3] = hi.uw[3];
        break;
    case 2: {
        s64 v0 = (s64)(((u64)hi.uw[0] << 32) | lo.uw[0]);
        s64 v1 = (s64)(((u64)hi.uw[2] << 32) | lo.uw[2]);
        o.sd[0] = ps2_sat_s32(v0);
        o.sd[1] = ps2_sat_s32(v1);
        break;
    }
    case 3:
        o.uh[0] = lo.uh[0]; o.uh[1] = lo.uh[2];
        o.uh[2] = hi.uh[0]; o.uh[3] = hi.uh[2];
        o.uh[4] = lo.uh[4]; o.uh[5] = lo.uh[6];
        o.uh[6] = hi.uh[4]; o.uh[7] = hi.uh[6];
        break;
    case 4:
        o.sh[0] = ps2_sat_s16(lo.sw[0]); o.sh[1] = ps2_sat_s16(lo.sw[1]);
        o.sh[2] = ps2_sat_s16(hi.sw[0]); o.sh[3] = ps2_sat_s16(hi.sw[1]);
        o.sh[4] = ps2_sat_s16(lo.sw[2]); o.sh[5] = ps2_sat_s16(lo.sw[3]);
        o.sh[6] = ps2_sat_s16(hi.sw[2]); o.sh[7] = ps2_sat_s16(hi.sw[3]);
        break;
    default:
        o = lo;
        break;
    }
    ctx->r[rd] = o;
}
PS2_INLINE void ps2_pmthl(ps2_ctx *ctx, int rs, int sa) {
    (void)sa;
    ps2_reg128 a = ctx->r[rs];
    ctx->lo.uw[0] = a.uw[0]; ctx->hi.uw[0] = a.uw[1];
    ctx->lo.uw[2] = a.uw[2]; ctx->hi.uw[2] = a.uw[3];
}

PS2_INLINE void ps2_pmulth(ps2_ctx *ctx, int rd, int rs, int rt) {
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt];
    s32 p[8];
    for (int i = 0; i < 8; i++) p[i] = (s32)a.sh[i] * (s32)b.sh[i];
    ctx->lo.uw[0] = (u32)p[0]; ctx->lo.uw[1] = (u32)p[1];
    ctx->hi.uw[0] = (u32)p[2]; ctx->hi.uw[1] = (u32)p[3];
    ctx->lo.uw[2] = (u32)p[4]; ctx->lo.uw[3] = (u32)p[5];
    ctx->hi.uw[2] = (u32)p[6]; ctx->hi.uw[3] = (u32)p[7];
    if (rd) {
        ctx->r[rd].sw[0] = p[0]; ctx->r[rd].sw[1] = p[2];
        ctx->r[rd].sw[2] = p[4]; ctx->r[rd].sw[3] = p[6];
    }
}
PS2_INLINE void ps2_phmadh(ps2_ctx *ctx, int rd, int rs, int rt) {
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt];
    s32 p[8], res[4];
    for (int i = 0; i < 8; i++) p[i] = (s32)a.sh[i] * (s32)b.sh[i];
    for (int i = 0; i < 4; i++) res[i] = p[i * 2 + 1] + p[i * 2];
    ctx->lo.uw[0] = (u32)res[0]; ctx->lo.uw[1] = (u32)p[1];
    ctx->hi.uw[0] = (u32)res[1]; ctx->hi.uw[1] = (u32)p[3];
    ctx->lo.uw[2] = (u32)res[2]; ctx->lo.uw[3] = (u32)p[5];
    ctx->hi.uw[2] = (u32)res[3]; ctx->hi.uw[3] = (u32)p[7];
    if (rd) for (int i = 0; i < 4; i++) ctx->r[rd].sw[i] = res[i];
}
PS2_INLINE void ps2_phmsbh(ps2_ctx *ctx, int rd, int rs, int rt) {
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt];
    s32 p[8], res[4];
    for (int i = 0; i < 8; i++) p[i] = (s32)a.sh[i] * (s32)b.sh[i];
    for (int i = 0; i < 4; i++) res[i] = p[i * 2 + 1] - p[i * 2];
    ctx->lo.uw[0] = (u32)res[0]; ctx->lo.uw[1] = (u32)~p[1];
    ctx->hi.uw[0] = (u32)res[1]; ctx->hi.uw[1] = (u32)~p[3];
    ctx->lo.uw[2] = (u32)res[2]; ctx->lo.uw[3] = (u32)~p[5];
    ctx->hi.uw[2] = (u32)res[3]; ctx->hi.uw[3] = (u32)~p[7];
    if (rd) for (int i = 0; i < 4; i++) ctx->r[rd].sw[i] = res[i];
}
PS2_INLINE void ps2_pmaddh(ps2_ctx *ctx, int rd, int rs, int rt) {
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt];
    s32 acc[8];
    acc[0] = (s32)ctx->lo.uw[0]; acc[1] = (s32)ctx->lo.uw[1];
    acc[2] = (s32)ctx->hi.uw[0]; acc[3] = (s32)ctx->hi.uw[1];
    acc[4] = (s32)ctx->lo.uw[2]; acc[5] = (s32)ctx->lo.uw[3];
    acc[6] = (s32)ctx->hi.uw[2]; acc[7] = (s32)ctx->hi.uw[3];
    for (int i = 0; i < 8; i++) acc[i] += (s32)a.sh[i] * (s32)b.sh[i];
    ctx->lo.uw[0] = (u32)acc[0]; ctx->lo.uw[1] = (u32)acc[1];
    ctx->hi.uw[0] = (u32)acc[2]; ctx->hi.uw[1] = (u32)acc[3];
    ctx->lo.uw[2] = (u32)acc[4]; ctx->lo.uw[3] = (u32)acc[5];
    ctx->hi.uw[2] = (u32)acc[6]; ctx->hi.uw[3] = (u32)acc[7];
    if (rd) {
        ctx->r[rd].sw[0] = acc[0]; ctx->r[rd].sw[1] = acc[2];
        ctx->r[rd].sw[2] = acc[4]; ctx->r[rd].sw[3] = acc[6];
    }
}
PS2_INLINE void ps2_pmsubh(ps2_ctx *ctx, int rd, int rs, int rt) {
    ps2_reg128 a = ctx->r[rs], b = ctx->r[rt];
    s32 acc[8];
    acc[0] = (s32)ctx->lo.uw[0]; acc[1] = (s32)ctx->lo.uw[1];
    acc[2] = (s32)ctx->hi.uw[0]; acc[3] = (s32)ctx->hi.uw[1];
    acc[4] = (s32)ctx->lo.uw[2]; acc[5] = (s32)ctx->lo.uw[3];
    acc[6] = (s32)ctx->hi.uw[2]; acc[7] = (s32)ctx->hi.uw[3];
    for (int i = 0; i < 8; i++) acc[i] -= (s32)a.sh[i] * (s32)b.sh[i];
    ctx->lo.uw[0] = (u32)acc[0]; ctx->lo.uw[1] = (u32)acc[1];
    ctx->hi.uw[0] = (u32)acc[2]; ctx->hi.uw[1] = (u32)acc[3];
    ctx->lo.uw[2] = (u32)acc[4]; ctx->lo.uw[3] = (u32)acc[5];
    ctx->hi.uw[2] = (u32)acc[6]; ctx->hi.uw[3] = (u32)acc[7];
    if (rd) {
        ctx->r[rd].sw[0] = acc[0]; ctx->r[rd].sw[1] = acc[2];
        ctx->r[rd].sw[2] = acc[4]; ctx->r[rd].sw[3] = acc[6];
    }
}
PS2_INLINE void ps2_pmultw(ps2_ctx *ctx, int rd, int rs, int rt) {
    s64 p0 = (s64)ctx->r[rs].sw[0] * (s64)ctx->r[rt].sw[0];
    s64 p1 = (s64)ctx->r[rs].sw[2] * (s64)ctx->r[rt].sw[2];
    ctx->lo.sd[0] = (s32)(u32)p0; ctx->hi.sd[0] = (s32)(u32)((u64)p0 >> 32);
    ctx->lo.sd[1] = (s32)(u32)p1; ctx->hi.sd[1] = (s32)(u32)((u64)p1 >> 32);
    if (rd) { ctx->r[rd].sd[0] = p0; ctx->r[rd].sd[1] = p1; }
}
PS2_INLINE void ps2_pmultuw(ps2_ctx *ctx, int rd, int rs, int rt) {
    u64 p0 = (u64)ctx->r[rs].uw[0] * (u64)ctx->r[rt].uw[0];
    u64 p1 = (u64)ctx->r[rs].uw[2] * (u64)ctx->r[rt].uw[2];
    ctx->lo.sd[0] = (s32)(u32)p0; ctx->hi.sd[0] = (s32)(u32)(p0 >> 32);
    ctx->lo.sd[1] = (s32)(u32)p1; ctx->hi.sd[1] = (s32)(u32)(p1 >> 32);
    if (rd) { ctx->r[rd].ud[0] = p0; ctx->r[rd].ud[1] = p1; }
}
PS2_INLINE void ps2_pmaddw(ps2_ctx *ctx, int rd, int rs, int rt) {
    s64 a0 = (s64)(((u64)ctx->hi.uw[0] << 32) | ctx->lo.uw[0]);
    s64 a1 = (s64)(((u64)ctx->hi.uw[2] << 32) | ctx->lo.uw[2]);
    s64 p0 = a0 + (s64)ctx->r[rs].sw[0] * (s64)ctx->r[rt].sw[0];
    s64 p1 = a1 + (s64)ctx->r[rs].sw[2] * (s64)ctx->r[rt].sw[2];
    ctx->lo.sd[0] = (s32)(u32)p0; ctx->hi.sd[0] = (s32)(u32)((u64)p0 >> 32);
    ctx->lo.sd[1] = (s32)(u32)p1; ctx->hi.sd[1] = (s32)(u32)((u64)p1 >> 32);
    if (rd) { ctx->r[rd].sd[0] = p0; ctx->r[rd].sd[1] = p1; }
}
PS2_INLINE void ps2_pmadduw(ps2_ctx *ctx, int rd, int rs, int rt) {
    u64 a0 = ((u64)ctx->hi.uw[0] << 32) | ctx->lo.uw[0];
    u64 a1 = ((u64)ctx->hi.uw[2] << 32) | ctx->lo.uw[2];
    u64 p0 = a0 + (u64)ctx->r[rs].uw[0] * (u64)ctx->r[rt].uw[0];
    u64 p1 = a1 + (u64)ctx->r[rs].uw[2] * (u64)ctx->r[rt].uw[2];
    ctx->lo.sd[0] = (s32)(u32)p0; ctx->hi.sd[0] = (s32)(u32)(p0 >> 32);
    ctx->lo.sd[1] = (s32)(u32)p1; ctx->hi.sd[1] = (s32)(u32)(p1 >> 32);
    if (rd) { ctx->r[rd].ud[0] = p0; ctx->r[rd].ud[1] = p1; }
}
PS2_INLINE void ps2_pmsubw(ps2_ctx *ctx, int rd, int rs, int rt) {
    s64 a0 = (s64)(((u64)ctx->hi.uw[0] << 32) | ctx->lo.uw[0]);
    s64 a1 = (s64)(((u64)ctx->hi.uw[2] << 32) | ctx->lo.uw[2]);
    s64 p0 = a0 - (s64)ctx->r[rs].sw[0] * (s64)ctx->r[rt].sw[0];
    s64 p1 = a1 - (s64)ctx->r[rs].sw[2] * (s64)ctx->r[rt].sw[2];
    ctx->lo.sd[0] = (s32)(u32)p0; ctx->hi.sd[0] = (s32)(u32)((u64)p0 >> 32);
    ctx->lo.sd[1] = (s32)(u32)p1; ctx->hi.sd[1] = (s32)(u32)((u64)p1 >> 32);
    if (rd) { ctx->r[rd].sd[0] = p0; ctx->r[rd].sd[1] = p1; }
}
PS2_INLINE void ps2_pdivw(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rd;
    for (int i = 0; i < 2; i++) {
        s32 n = ctx->r[rs].sw[i * 2], d = ctx->r[rt].sw[i * 2];
        s32 q, r;
        if (d == 0) { q = n < 0 ? 1 : -1; r = n; }
        else if ((u32)n == 0x80000000u && d == -1) { q = (s32)0x80000000; r = 0; }
        else { q = n / d; r = n % d; }
        ctx->lo.sd[i] = q;
        ctx->hi.sd[i] = r;
    }
}
PS2_INLINE void ps2_pdivuw(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rd;
    for (int i = 0; i < 2; i++) {
        u32 n = ctx->r[rs].uw[i * 2], d = ctx->r[rt].uw[i * 2];
        u32 q = d ? n / d : 0xFFFFFFFFu;
        u32 r = d ? n % d : n;
        ctx->lo.sd[i] = (s32)q;
        ctx->hi.sd[i] = (s32)r;
    }
}
PS2_INLINE void ps2_pdivbw(ps2_ctx *ctx, int rd, int rs, int rt) {
    (void)rd;
    s16 d = ctx->r[rt].sh[0];
    for (int i = 0; i < 4; i++) {
        s32 n = ctx->r[rs].sw[i];
        s32 q, r;
        if (d == 0) { q = n < 0 ? 1 : -1; r = n; }
        else if ((u32)n == 0x80000000u && d == -1) { q = (s32)0x80000000; r = 0; }
        else { q = n / d; r = n % d; }
        ctx->lo.sw[i] = q;
        ctx->hi.sw[i] = (s16)r;
    }
}

#undef MMI_RD
#undef MMI_GUARD

#ifdef __cplusplus
}
#endif
#endif
