#ifndef PS2_VU0_H
#define PS2_VU0_H

#ifndef PS2_RUNTIME_H
#error "include ps2_runtime.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

PS2_INLINE void ps2_vu_store(ps2_vu *vu, int reg, int dest, const float v[4]) {
    ps2_vf *d;
    if (reg == 0) return;
    d = &vu->vf[reg];
    if (dest & 8) d->f[0] = v[0];
    if (dest & 4) d->f[1] = v[1];
    if (dest & 2) d->f[2] = v[2];
    if (dest & 1) d->f[3] = v[3];
}
PS2_INLINE void ps2_vu_store_acc(ps2_vu *vu, int dest, const float v[4]) {
    if (dest & 8) vu->acc.f[0] = v[0];
    if (dest & 4) vu->acc.f[1] = v[1];
    if (dest & 2) vu->acc.f[2] = v[2];
    if (dest & 1) vu->acc.f[3] = v[3];
}

PS2_INLINE void ps2_vu0_advance(ps2_ctx *ctx, u32 slots) {
    if (!ctx->vu0_flag_pending) return;
    if (slots > 5) slots = 5;
    while (slots--) {
        u32 s = ctx->vu0_flag_slot + 1;
        if (s == 5) s = 0;
        ctx->vu0_flag_slot = s;
        if (ctx->vu0_flag_pending & (1u << s)) {
            u32 mac = ctx->vu0_flags[s].mac, st = 0;
            ctx->vu0.mac = mac;
            if (mac & 0x000F) st |= 1;
            if (mac & 0x00F0) st |= 2;
            if (mac & 0x0F00) st |= 4;
            if (mac & 0xF000) st |= 8;
            ctx->vu0.status = (ctx->vu0.status & 0xFF0u) | st | (st << 6);
            ctx->vu0_flag_pending &= ~(1u << s);
        }
    }
}

PS2_INLINE void ps2_vu_macflag(ps2_ctx *ctx, int dest, const double v[4]) {
    u32 mac = 0;
    for (int i = 0; i < 4; i++) {
        int bit = 3 - i;
        if (!((dest >> bit) & 1)) continue;
        float f = (float)v[i];
        u32 u;
        memcpy(&u, &f, 4);
        double a = v[i] < 0 ? -v[i] : v[i];
        if (a == 0 || a < 0x1p-126) mac |= 1u << bit;
        if (u & 0x80000000u) mac |= 1u << (bit + 4);
        if (a != 0 && a < 0x1p-126) mac |= 1u << (bit + 8);
        if (a > 0x1.fffffep127) mac |= 1u << (bit + 12);
    }
    ctx->vu0_flags[ctx->vu0_flag_slot].mac = mac;
    ctx->vu0_flag_pending |= 1u << ctx->vu0_flag_slot;
}

PS2_INLINE void ps2_vu0_add(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_add_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_add_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_add_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_sub(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_sub_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_sub_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_sub_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mul(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mul_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mul_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mul_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_max(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_vu_select(a, b, 0);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_max_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_vu_select(a, b, 0);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_max_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_vu_select(a, b, 0);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_max_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_vu_select(a, b, 0);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mini(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_vu_select(a, b, 1);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mini_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_vu_select(a, b, 1);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mini_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_vu_select(a, b, 1);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_mini_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_vu_select(a, b, 1);
    }
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_madd(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_madd_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_madd_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_madd_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_msub(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_msub_bc(ps2_ctx *ctx, int dest, int fd, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_msub_q(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_msub_i(ps2_ctx *ctx, int dest, int fd, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store(vu, fd, dest, r);
}

PS2_INLINE void ps2_vu0_adda(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_adda_bc(ps2_ctx *ctx, int dest, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_adda_q(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_adda_i(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fadd(a,b);
        raw[i] = (double)ps2_ftz(a)+ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_suba(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_suba_bc(ps2_ctx *ctx, int dest, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_suba_q(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_suba_i(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fsub(a,b);
        raw[i] = (double)ps2_ftz(a)-ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_mula(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_mula_bc(ps2_ctx *ctx, int dest, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_mula_q(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_mula_i(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fmul(a,b);
        raw[i] = (double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_madda(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_madda_bc(ps2_ctx *ctx, int dest, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_madda_q(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_madda_i(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fadd(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])+(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_msuba(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[i];
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_msuba_bc(ps2_ctx *ctx, int dest, int fs, int ft, int bc) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->vf[ft].f[bc];
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_msuba_q(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->q;
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_msuba_i(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4];
    for (int i = 0; i < 4; i++) {
        float a = vu->vf[fs].f[i], b = vu->i;
        r[i] = ps2_fsub(vu->acc.f[i],ps2_fmul(a,b));
        raw[i] = ps2_ftz(vu->acc.f[i])-(double)ps2_ftz(a)*ps2_ftz(b);
    }
    ps2_vu_macflag(ctx, dest, raw);
    ps2_vu_store_acc(vu, dest, r);
}

PS2_INLINE void ps2_vu0_opmula(ps2_ctx *ctx, int dest, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    double raw[4] = {0};
    for (int i = 0; i < 3; i++)
        raw[i] = (double)ps2_ftz(vu->vf[fs].f[(i+1)%3]) * ps2_ftz(vu->vf[ft].f[(i+2)%3]);
    ps2_vu_macflag(ctx, 0xE, raw);
    (void)dest;
    vu->acc.f[0] = ps2_fmul(vu->vf[fs].f[1], vu->vf[ft].f[2]);
    vu->acc.f[1] = ps2_fmul(vu->vf[fs].f[2], vu->vf[ft].f[0]);
    vu->acc.f[2] = ps2_fmul(vu->vf[fs].f[0], vu->vf[ft].f[1]);
}
PS2_INLINE void ps2_vu0_opmsub(ps2_ctx *ctx, int dest, int fd, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    double raw[4] = {0};
    for (int i = 0; i < 3; i++)
        raw[i] = (double)ps2_ftz(vu->acc.f[i]) -
            (double)ps2_ftz(vu->vf[fs].f[(i+1)%3]) * ps2_ftz(vu->vf[ft].f[(i+2)%3]);
    ps2_vu_macflag(ctx, 0xE, raw);
    (void)dest;
    r[0] = ps2_fsub(vu->acc.f[0], ps2_fmul(vu->vf[fs].f[1], vu->vf[ft].f[2]));
    r[1] = ps2_fsub(vu->acc.f[1], ps2_fmul(vu->vf[fs].f[2], vu->vf[ft].f[0]));
    r[2] = ps2_fsub(vu->acc.f[2], ps2_fmul(vu->vf[fs].f[0], vu->vf[ft].f[1]));
    r[3] = vu->vf[fd].f[3];
    ps2_vu_store(vu, fd, 0xE, r);
}

PS2_INLINE void ps2_vu0_move(ps2_ctx *ctx, int dest, int ft, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = vu->vf[fs].f[i];
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE void ps2_vu0_mr32(ps2_ctx *ctx, int dest, int ft, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    r[0] = vu->vf[fs].f[1];
    r[1] = vu->vf[fs].f[2];
    r[2] = vu->vf[fs].f[3];
    r[3] = vu->vf[fs].f[0];
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE void ps2_vu0_abs(ps2_ctx *ctx, int dest, int ft, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    for (int i = 0; i < 4; i++) {
        u32 u = vu->vf[fs].u[i] & 0x7FFFFFFFu;
        memcpy(&r[i], &u, 4);
    }
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE void ps2_vu0_ftoi(ps2_ctx *ctx, int dest, int ft, int fs, int bits) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    ps2_vf tmp;
    float scale = (float)(1u << bits);
    for (int i = 0; i < 4; i++)
        tmp.s[i] = ps2_cvt_w_s(vu->vf[fs].f[i] * scale);
    for (int i = 0; i < 4; i++) memcpy(&r[i], &tmp.s[i], 4);
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE void ps2_vu0_itof(ps2_ctx *ctx, int dest, int ft, int fs, int bits) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    float scale = 1.0f / (float)(1u << bits);
    for (int i = 0; i < 4; i++) r[i] = (float)vu->vf[fs].s[i] * scale;
    ps2_vu_store(vu, ft, dest, r);
}

void ps2_vu0_div(ps2_ctx *ctx, int fs, int fsf, int ft, int ftf);
void ps2_vu0_sqrt(ps2_ctx *ctx, int ft, int ftf);
void ps2_vu0_rsqrt(ps2_ctx *ctx, int fs, int fsf, int ft, int ftf);

PS2_INLINE void ps2_vu0_clip(ps2_ctx *ctx, int fs, int ft) {
    ps2_vu *vu = &ctx->vu0;
    u32 c = (vu->clip << 6) & 0xFFFFFFu;
    float w = vu->vf[ft].f[3];
    float aw = w < 0.0f ? -w : w;
    for (int i = 0; i < 3; i++) {
        float v = vu->vf[fs].f[i];
        if (v > aw)  c |= 1u << (i * 2);
        if (v < -aw) c |= 1u << (i * 2 + 1);
    }
    vu->clip = c & 0xFFFFFFu;
}

PS2_INLINE void ps2_vu0_mfir(ps2_ctx *ctx, int dest, int ft, int fs) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    s32 v = (s32)(s16)vu->vi[fs & 15];
    for (int i = 0; i < 4; i++) memcpy(&r[i], &v, 4);
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE u16 ps2_vu0_ilwr(ps2_ctx *ctx, int dest, int fs) {
    ps2_vu *vu = &ctx->vu0;
    u32 base = (u32)vu->vi[fs & 15] * 16u;
    int lane = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
    u32 off = (base + (u32)lane * 4u) & (vu->mem_size - 1u);
    u16 v;
    memcpy(&v, vu->mem + off, 2);
    return v;
}
PS2_INLINE void ps2_vu0_iswr(ps2_ctx *ctx, int dest, int it, int fs) {
    ps2_vu *vu = &ctx->vu0;
    u32 base = (u32)vu->vi[fs & 15] * 16u;
    u32 v = vu->vi[it & 15];
    for (int i = 0; i < 4; i++) {
        if (!((dest >> (3 - i)) & 1)) continue;
        u32 off = (base + (u32)i * 4u) & (vu->mem_size - 1u);
        memcpy(vu->mem + off, &v, 4);
    }
}
PS2_INLINE void ps2_vu0_lq_at(ps2_ctx *ctx, int dest, int ft, u32 addr) {
    ps2_vu *vu = &ctx->vu0;
    float r[4];
    u32 off = addr & (vu->mem_size - 16u);
    memcpy(r, vu->mem + off, 16);
    ps2_vu_store(vu, ft, dest, r);
}
PS2_INLINE void ps2_vu0_sq_at(ps2_ctx *ctx, int dest, int fs, u32 addr) {
    ps2_vu *vu = &ctx->vu0;
    u32 off = addr & (vu->mem_size - 16u);
    for (int i = 0; i < 4; i++) {
        if (!((dest >> (3 - i)) & 1)) continue;
        memcpy(vu->mem + off + (u32)i * 4u, &vu->vf[fs].f[i], 4);
    }
}
PS2_INLINE void ps2_vu0_lqi(ps2_ctx *ctx, int dest, int ft, int is) {
    ps2_vu *vu = &ctx->vu0;
    ps2_vu0_lq_at(ctx, dest, ft, (u32)vu->vi[is & 15] * 16u);
    if ((is & 15) != 0) vu->vi[is & 15]++;
}
PS2_INLINE void ps2_vu0_lqd(ps2_ctx *ctx, int dest, int ft, int is) {
    ps2_vu *vu = &ctx->vu0;
    if ((is & 15) != 0) vu->vi[is & 15]--;
    ps2_vu0_lq_at(ctx, dest, ft, (u32)vu->vi[is & 15] * 16u);
}
PS2_INLINE void ps2_vu0_sqi(ps2_ctx *ctx, int dest, int fs, int it) {
    ps2_vu *vu = &ctx->vu0;
    ps2_vu0_sq_at(ctx, dest, fs, (u32)vu->vi[it & 15] * 16u);
    if ((it & 15) != 0) vu->vi[it & 15]++;
}
PS2_INLINE void ps2_vu0_sqd(ps2_ctx *ctx, int dest, int fs, int it) {
    ps2_vu *vu = &ctx->vu0;
    if ((it & 15) != 0) vu->vi[it & 15]--;
    ps2_vu0_sq_at(ctx, dest, fs, (u32)vu->vi[it & 15] * 16u);
}

void ps2_vu0_rnext(ps2_ctx *ctx, int dest, int ft);
void ps2_vu0_rget(ps2_ctx *ctx, int dest, int ft);
void ps2_vu0_rinit(ps2_ctx *ctx, int fs, int fsf);
void ps2_vu0_rxor(ps2_ctx *ctx, int fs, int fsf);

u32  ps2_vu0_cfc(ps2_ctx *ctx, int reg);
void ps2_vu0_ctc(ps2_ctx *ctx, int reg, u32 val);
void ps2_vu0_callms(ps2_ctx *ctx, u32 addr);

#ifdef __cplusplus
}
#endif
#endif
