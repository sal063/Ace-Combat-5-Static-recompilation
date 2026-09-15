import os
from . import r5900
from .r5900 import (CAT_BRANCH, CAT_BRANCH_LIKELY, CAT_JUMP, CAT_JUMPR,
                    CAT_SYSCALL, CAT_TRAP)


def fname(addr: int) -> str:
    return "func_%08X" % addr


def label(addr: int) -> str:
    return "L_%08X" % addr


def ru32(n):
    return "0u" if n == 0 else "ctx->r[%d].uw[0]" % n


def rs32(n):
    return "0" if n == 0 else "ctx->r[%d].sw[0]" % n


def ru64(n):
    return "0ull" if n == 0 else "ctx->r[%d].ud[0]" % n


def rs64(n):
    return "0ll" if n == 0 else "ctx->r[%d].sd[0]" % n


def rq(n):
    return "ps2_zero_q" if n == 0 else "ctx->r[%d]" % n


def wr64(n, expr):
    if n == 0:
        return None
    return "ctx->r[%d].ud[0] = (u64)(%s);" % (n, expr)


def wrs32(n, expr):
    if n == 0:
        return None
    return "ctx->r[%d].sd[0] = (s64)(s32)(%s);" % (n, expr)


def wrq(n, expr):
    if n == 0:
        return None
    return "ctx->r[%d] = %s;" % (n, expr)


def addr_expr(base, simm):
    if base == 0:
        return "0x%08Xu" % (simm & 0xFFFFFFFF)
    if simm == 0:
        return ru32(base)
    if simm > 0:
        return "(%s + 0x%Xu)" % (ru32(base), simm)
    return "(%s - 0x%Xu)" % (ru32(base), -simm)


FCC = "ctx->fcc"


def f_u(n):
    return "ctx->f[%d].u" % n


def f_f(n):
    return "ctx->f[%d].f" % n


class Emitter:
    def __init__(self, prog, opts=None):
        self.p = prog
        self.opts = opts or {}
        self.comments = self.opts.get("comments", False)
        self.warnings = []
        self.unhandled = {}
        self.symbols = dict(self.opts.get("symbols") or {})
        self.overrides = dict(self.opts.get("overrides") or {})
        self.overrides_used = {}
        self.hooks = dict(self.opts.get("hooks") or {})
        self.ee_slots = 0

    def flush_slots(self, out, ind=1):
        if self.ee_slots:
            self.w(out, "ps2_vu0_advance(ctx, %du);" % min(self.ee_slots, 5), ind)
            self.ee_slots = 0

    def sym(self, addr):
        return self.symbols.get(addr) or self.p.names.get(addr)

    def w(self, out, s, ind=1):
        out.append("    " * ind + s)

    def warn(self, msg):
        self.warnings.append(msg)

    def emit_insn(self, ins, out, ind=1):
        self.ee_slots += 1
        if ins.op == 0x12 or ins.name in ("LQC2", "SQC2") or ins.cat in (CAT_SYSCALL, CAT_TRAP):
            self.flush_slots(out, ind)
        n = ins.name
        h = getattr(self, "i_" + n.replace(".", "_"), None)
        if h is None:
            self.unhandled[n] = self.unhandled.get(n, 0) + 1
            self.w(out, "ps2_unimplemented(ctx, 0x%08Xu, 0x%08Xu); /* %s */"
                   % (ins.addr, ins.word, n), ind)
            return
        res = h(ins)
        if res is None:
            return
        if isinstance(res, str):
            res = [res]
        for line in res:
            if line:
                self.w(out, line, ind)

    def i_NOP(self, i):
        return None

    def i_SYNC(self, i):
        return None

    def i_CACHE(self, i):
        return None

    def i_PREF(self, i):
        return None

    def i_SLL(self, i):
        if i.word == 0:
            return None
        return wrs32(i.rd, "(u32)(%s) << %d" % (ru32(i.rt), i.sa))

    def i_SRL(self, i):
        return wrs32(i.rd, "(u32)(%s) >> %d" % (ru32(i.rt), i.sa))

    def i_SRA(self, i):
        return wrs32(i.rd, "(s32)(%s) >> %d" % (rs32(i.rt), i.sa))

    def i_SLLV(self, i):
        return wrs32(i.rd, "(u32)(%s) << (%s & 31)" % (ru32(i.rt), ru32(i.rs)))

    def i_SRLV(self, i):
        return wrs32(i.rd, "(u32)(%s) >> (%s & 31)" % (ru32(i.rt), ru32(i.rs)))

    def i_SRAV(self, i):
        return wrs32(i.rd, "(s32)(%s) >> (%s & 31)" % (rs32(i.rt), ru32(i.rs)))

    def i_DSLL(self, i):
        return wr64(i.rd, "%s << %d" % (ru64(i.rt), i.sa))

    def i_DSRL(self, i):
        return wr64(i.rd, "%s >> %d" % (ru64(i.rt), i.sa))

    def i_DSRA(self, i):
        return wr64(i.rd, "(s64)(%s) >> %d" % (rs64(i.rt), i.sa))

    def i_DSLL32(self, i):
        return wr64(i.rd, "%s << %d" % (ru64(i.rt), i.sa + 32))

    def i_DSRL32(self, i):
        return wr64(i.rd, "%s >> %d" % (ru64(i.rt), i.sa + 32))

    def i_DSRA32(self, i):
        return wr64(i.rd, "(s64)(%s) >> %d" % (rs64(i.rt), i.sa + 32))

    def i_DSLLV(self, i):
        return wr64(i.rd, "%s << (%s & 63)" % (ru64(i.rt), ru32(i.rs)))

    def i_DSRLV(self, i):
        return wr64(i.rd, "%s >> (%s & 63)" % (ru64(i.rt), ru32(i.rs)))

    def i_DSRAV(self, i):
        return wr64(i.rd, "(s64)(%s) >> (%s & 63)" % (rs64(i.rt), ru32(i.rs)))

    def i_ADD(self, i):
        return wrs32(i.rd, "(u32)(%s) + (u32)(%s)" % (ru32(i.rs), ru32(i.rt)))

    def i_ADDU(self, i):
        return self.i_ADD(i)

    def i_SUB(self, i):
        return wrs32(i.rd, "(u32)(%s) - (u32)(%s)" % (ru32(i.rs), ru32(i.rt)))

    def i_SUBU(self, i):
        return self.i_SUB(i)

    def i_DADD(self, i):
        return wr64(i.rd, "%s + %s" % (ru64(i.rs), ru64(i.rt)))

    def i_DADDU(self, i):
        return self.i_DADD(i)

    def i_DSUB(self, i):
        return wr64(i.rd, "%s - %s" % (ru64(i.rs), ru64(i.rt)))

    def i_DSUBU(self, i):
        return self.i_DSUB(i)

    def i_AND(self, i):
        return wr64(i.rd, "%s & %s" % (ru64(i.rs), ru64(i.rt)))

    def i_OR(self, i):
        return wr64(i.rd, "%s | %s" % (ru64(i.rs), ru64(i.rt)))

    def i_XOR(self, i):
        return wr64(i.rd, "%s ^ %s" % (ru64(i.rs), ru64(i.rt)))

    def i_NOR(self, i):
        return wr64(i.rd, "~(%s | %s)" % (ru64(i.rs), ru64(i.rt)))

    def i_SLT(self, i):
        return wr64(i.rd, "(s64)(%s) < (s64)(%s)" % (rs64(i.rs), rs64(i.rt)))

    def i_SLTU(self, i):
        return wr64(i.rd, "(u64)(%s) < (u64)(%s)" % (ru64(i.rs), ru64(i.rt)))

    def i_ADDI(self, i):
        return wrs32(i.rt, "(u32)(%s) + (u32)0x%08X"
                     % (ru32(i.rs), i.simm & 0xFFFFFFFF))

    def i_ADDIU(self, i):
        return self.i_ADDI(i)

    def i_DADDI(self, i):
        return wr64(i.rt, "%s + (s64)%d" % (ru64(i.rs), i.simm))

    def i_DADDIU(self, i):
        return self.i_DADDI(i)

    def i_SLTI(self, i):
        return wr64(i.rt, "(s64)(%s) < (s64)%d" % (rs64(i.rs), i.simm))

    def i_SLTIU(self, i):
        return wr64(i.rt, "(u64)(%s) < (u64)(s64)%d" % (ru64(i.rs), i.simm))

    def i_ANDI(self, i):
        return wr64(i.rt, "%s & 0x%Xull" % (ru64(i.rs), i.imm))

    def i_ORI(self, i):
        return wr64(i.rt, "%s | 0x%Xull" % (ru64(i.rs), i.imm))

    def i_XORI(self, i):
        return wr64(i.rt, "%s ^ 0x%Xull" % (ru64(i.rs), i.imm))

    def i_LUI(self, i):
        return wrs32(i.rt, "0x%08Xu" % ((i.imm << 16) & 0xFFFFFFFF))

    def i_MOVZ(self, i):
        if i.rd == 0:
            return None
        return "if (%s == 0) { %s }" % (ru64(i.rt), wr64(i.rd, ru64(i.rs)))

    def i_MOVN(self, i):
        if i.rd == 0:
            return None
        return "if (%s != 0) { %s }" % (ru64(i.rt), wr64(i.rd, ru64(i.rs)))

    def _trap(self, i, cond=None):
        body = "ctx->pc = 0x%08Xu; ps2_trap(ctx, 0x%08Xu);" % (i.addr, i.word)
        if cond is None:
            return body
        return "if (%s) { %s }" % (cond, body)

    def i_BREAK(self, i):
        return self._trap(i)

    def i_TGE(self, i):
        return self._trap(i, "(s64)(%s) >= (s64)(%s)" % (rs64(i.rs), rs64(i.rt)))

    def i_TGEU(self, i):
        return self._trap(i, "(u64)(%s) >= (u64)(%s)" % (ru64(i.rs), ru64(i.rt)))

    def i_TLT(self, i):
        return self._trap(i, "(s64)(%s) < (s64)(%s)" % (rs64(i.rs), rs64(i.rt)))

    def i_TLTU(self, i):
        return self._trap(i, "(u64)(%s) < (u64)(%s)" % (ru64(i.rs), ru64(i.rt)))

    def i_TEQ(self, i):
        return self._trap(i, "%s == %s" % (ru64(i.rs), ru64(i.rt)))

    def i_TNE(self, i):
        return self._trap(i, "%s != %s" % (ru64(i.rs), ru64(i.rt)))

    def i_TGEI(self, i):
        return self._trap(i, "(s64)(%s) >= (s64)%d" % (rs64(i.rs), i.simm))

    def i_TGEIU(self, i):
        return self._trap(i, "(u64)(%s) >= (u64)(s64)%d" % (ru64(i.rs), i.simm))

    def i_TLTI(self, i):
        return self._trap(i, "(s64)(%s) < (s64)%d" % (rs64(i.rs), i.simm))

    def i_TLTIU(self, i):
        return self._trap(i, "(u64)(%s) < (u64)(s64)%d" % (ru64(i.rs), i.simm))

    def i_TEQI(self, i):
        return self._trap(i, "%s == (u64)(s64)%d" % (ru64(i.rs), i.simm))

    def i_TNEI(self, i):
        return self._trap(i, "%s != (u64)(s64)%d" % (ru64(i.rs), i.simm))

    def i_SYSCALL(self, i):
        return "ctx->pc = 0x%08Xu; ps2_syscall(ctx);" % i.addr

    def i_MFHI(self, i):
        return wr64(i.rd, "ctx->hi.ud[0]")

    def i_MFLO(self, i):
        return wr64(i.rd, "ctx->lo.ud[0]")

    def i_MTHI(self, i):
        return "ctx->hi.ud[0] = %s;" % ru64(i.rs)

    def i_MTLO(self, i):
        return "ctx->lo.ud[0] = %s;" % ru64(i.rs)

    def i_MFHI1(self, i):
        return wr64(i.rd, "ctx->hi.ud[1]")

    def i_MFLO1(self, i):
        return wr64(i.rd, "ctx->lo.ud[1]")

    def i_MTHI1(self, i):
        return "ctx->hi.ud[1] = %s;" % ru64(i.rs)

    def i_MTLO1(self, i):
        return "ctx->lo.ud[1] = %s;" % ru64(i.rs)

    def i_MFSA(self, i):
        return wr64(i.rd, "ctx->sa")

    def i_MTSA(self, i):
        return "ctx->sa = (u32)(%s);" % ru32(i.rs)

    def i_MTSAB(self, i):
        return "ctx->sa = (((u32)(%s) & 0xF) ^ (0x%Xu & 0xF)) << 3;" % (
            ru32(i.rs), i.imm)

    def i_MTSAH(self, i):
        return "ctx->sa = (((u32)(%s) & 0x7) ^ (0x%Xu & 0x7)) << 4;" % (
            ru32(i.rs), i.imm)

    def _mul(self, i, pipe, signed):
        p = pipe
        cast = "s64" if signed else "u64"
        src = "(%s)(%s)(%s) * (%s)(%s)(%s)" % (
            cast, "s32" if signed else "u32", ru32(i.rs),
            cast, "s32" if signed else "u32", ru32(i.rt))
        lines = ["{ %s _p = %s;" % (cast, src),
                 "  ctx->lo.sd[%d] = (s64)(s32)(u32)_p;" % p,
                 "  ctx->hi.sd[%d] = (s64)(s32)(u32)(_p >> 32);" % p]
        w = wr64(i.rd, "ctx->lo.ud[%d]" % p)
        if w:
            lines.append("  " + w)
        lines.append("}")
        return lines

    def i_MULT(self, i):
        return self._mul(i, 0, True)

    def i_MULTU(self, i):
        return self._mul(i, 0, False)

    def i_MULT1(self, i):
        return self._mul(i, 1, True)

    def i_MULTU1(self, i):
        return self._mul(i, 1, False)

    def _madd(self, i, pipe, signed):
        cast = "s64" if signed else "u64"
        icast = "s32" if signed else "u32"
        return [
            "{ %s _p = (%s)(%s)(%s) * (%s)(%s)(%s);" % (
                cast, cast, icast, ru32(i.rs), cast, icast, ru32(i.rt)),
            "  u64 _acc = ((u64)(u32)ctx->hi.uw[%d] << 32) | (u32)ctx->lo.uw[%d];"
            % (pipe * 2, pipe * 2),
            "  _acc += (u64)_p;",
            "  ctx->lo.sd[%d] = (s64)(s32)(u32)_acc;" % pipe,
            "  ctx->hi.sd[%d] = (s64)(s32)(u32)(_acc >> 32);" % pipe,
        ] + ([("  " + wr64(i.rd, "ctx->lo.ud[%d]" % pipe))] if i.rd else []) + ["}"]

    def i_MADD(self, i):
        return self._madd(i, 0, True)

    def i_MADDU(self, i):
        return self._madd(i, 0, False)

    def i_MADD1(self, i):
        return self._madd(i, 1, True)

    def i_MADDU1(self, i):
        return self._madd(i, 1, False)

    def _div(self, i, pipe, signed):
        p = pipe
        if signed:
            return [
                "ps2_div(&ctx->lo, &ctx->hi, %d, %s, %s);" % (
                    p, rs32(i.rs), rs32(i.rt))]
        return ["ps2_divu(&ctx->lo, &ctx->hi, %d, %s, %s);" % (
            p, ru32(i.rs), ru32(i.rt))]

    def i_DIV(self, i):
        return self._div(i, 0, True)

    def i_DIVU(self, i):
        return self._div(i, 0, False)

    def i_DIV1(self, i):
        return self._div(i, 1, True)

    def i_DIVU1(self, i):
        return self._div(i, 1, False)

    def i_PLZCW(self, i):
        if i.rd == 0:
            return None
        return ["ctx->r[%d].uw[0] = ps2_plzcw(%s);" % (i.rd, ru32(i.rs)),
                "ctx->r[%d].uw[1] = ps2_plzcw(%s);" % (
                    i.rd, "0u" if i.rs == 0 else "ctx->r[%d].uw[1]" % i.rs)]

    def i_LB(self, i):
        return wr64(i.rt, "(s64)(s8)ps2_r8(%s)" % addr_expr(i.rs, i.simm))

    def i_LBU(self, i):
        return wr64(i.rt, "(u64)(u8)ps2_r8(%s)" % addr_expr(i.rs, i.simm))

    def i_LH(self, i):
        return wr64(i.rt, "(s64)(s16)ps2_r16(%s)" % addr_expr(i.rs, i.simm))

    def i_LHU(self, i):
        return wr64(i.rt, "(u64)(u16)ps2_r16(%s)" % addr_expr(i.rs, i.simm))

    def i_LW(self, i):
        return wr64(i.rt, "(s64)(s32)ps2_r32(%s)" % addr_expr(i.rs, i.simm))

    def i_LWU(self, i):
        return wr64(i.rt, "(u64)(u32)ps2_r32(%s)" % addr_expr(i.rs, i.simm))

    def i_LD(self, i):
        return wr64(i.rt, "ps2_r64(%s)" % addr_expr(i.rs, i.simm))

    def i_LQ(self, i):
        if i.rt == 0:
            return None
        return "ps2_r128(&ctx->r[%d], %s);" % (i.rt, addr_expr(i.rs, i.simm))

    def i_LWL(self, i):
        if i.rt == 0:
            return None
        return "ctx->r[%d].sd[0] = (s64)(s32)ps2_lwl(%s, %s);" % (
            i.rt, addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_LWR(self, i):
        if i.rt == 0:
            return None
        return "ctx->r[%d].sd[0] = (s64)(s32)ps2_lwr(%s, %s);" % (
            i.rt, addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_LDL(self, i):
        if i.rt == 0:
            return None
        return "ctx->r[%d].ud[0] = ps2_ldl(%s, %s);" % (
            i.rt, addr_expr(i.rs, i.simm), ru64(i.rt))

    def i_LDR(self, i):
        if i.rt == 0:
            return None
        return "ctx->r[%d].ud[0] = ps2_ldr(%s, %s);" % (
            i.rt, addr_expr(i.rs, i.simm), ru64(i.rt))

    def i_LWC1(self, i):
        return "%s = ps2_r32(%s);" % (f_u(i.ft), addr_expr(i.rs, i.simm))

    def i_LQC2(self, i):
        return "ps2_r128((ps2_reg128 *)&ctx->vu0.vf[%d], %s);" % (
            i.ft, addr_expr(i.rs, i.simm))

    def i_SB(self, i):
        return "ps2_w8(%s, (u8)%s);" % (addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_SH(self, i):
        return "ps2_w16(%s, (u16)%s);" % (addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_SW(self, i):
        return "ps2_w32(%s, (u32)%s);" % (addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_SD(self, i):
        return "ps2_w64(%s, %s);" % (addr_expr(i.rs, i.simm), ru64(i.rt))

    def i_SQ(self, i):
        return "ps2_w128(%s, &%s);" % (addr_expr(i.rs, i.simm), rq(i.rt))

    def i_SWL(self, i):
        return "ps2_swl(%s, %s);" % (addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_SWR(self, i):
        return "ps2_swr(%s, %s);" % (addr_expr(i.rs, i.simm), ru32(i.rt))

    def i_SDL(self, i):
        return "ps2_sdl(%s, %s);" % (addr_expr(i.rs, i.simm), ru64(i.rt))

    def i_SDR(self, i):
        return "ps2_sdr(%s, %s);" % (addr_expr(i.rs, i.simm), ru64(i.rt))

    def i_SWC1(self, i):
        return "ps2_w32(%s, %s);" % (addr_expr(i.rs, i.simm), f_u(i.ft))

    def i_SQC2(self, i):
        return "ps2_w128(%s, (const ps2_reg128 *)&ctx->vu0.vf[%d]);" % (
            addr_expr(i.rs, i.simm), i.ft)

    def i_MFC0(self, i):
        return wrs32(i.rt, "ps2_cop0_read(ctx, %d)" % i.rd)

    def i_MTC0(self, i):
        return "ps2_cop0_write(ctx, %d, %s);" % (i.rd, ru32(i.rt))

    def i_EI(self, i):
        return "ctx->cop0[12] |= 0x10000u;"

    def i_DI(self, i):
        return "ctx->cop0[12] &= ~0x10000u;"

    def i_ERET(self, i):
        return "ps2_eret(ctx);"

    def i_TLBR(self, i):
        return "ps2_tlb_op(ctx, 0);"

    def i_TLBWI(self, i):
        return "ps2_tlb_op(ctx, 1);"

    def i_TLBWR(self, i):
        return "ps2_tlb_op(ctx, 2);"

    def i_TLBP(self, i):
        return "ps2_tlb_op(ctx, 3);"

    def i_MFC1(self, i):
        return wrs32(i.rt, f_u(i.fs))

    def i_MTC1(self, i):
        return "%s = %s;" % (f_u(i.fs), ru32(i.rt))

    def i_CFC1(self, i):
        return wrs32(i.rt, "ps2_cfc1(ctx, %d)" % i.fs)

    def i_CTC1(self, i):
        return "ps2_ctc1(ctx, %d, %s);" % (i.fs, ru32(i.rt))

    def _f3(self, i, op):
        return "%s = %s(%s, %s);" % (f_f(i.fd), op, f_f(i.fs), f_f(i.ft))

    def i_ADD_S(self, i):
        return self._f3(i, "ps2_fadd")

    def i_SUB_S(self, i):
        return self._f3(i, "ps2_fsub")

    def i_MUL_S(self, i):
        return self._f3(i, "ps2_fmul")

    def i_DIV_S(self, i):
        return self._f3(i, "ps2_fdiv")

    def i_MAX_S(self, i):
        return self._f3(i, "ps2_fmax")

    def i_MIN_S(self, i):
        return self._f3(i, "ps2_fmin")

    def i_SQRT_S(self, i):
        return "%s = ps2_fsqrt(%s);" % (f_f(i.fd), f_f(i.ft))

    def i_RSQRT_S(self, i):
        return "%s = ps2_frsqrt(%s, %s);" % (f_f(i.fd), f_f(i.fs), f_f(i.ft))

    def i_ABS_S(self, i):
        return "%s = %s & 0x7FFFFFFFu;" % (f_u(i.fd), f_u(i.fs))

    def i_NEG_S(self, i):
        return "%s = %s ^ 0x80000000u;" % (f_u(i.fd), f_u(i.fs))

    def i_MOV_S(self, i):
        return "%s = %s;" % (f_u(i.fd), f_u(i.fs))

    def i_ADDA_S(self, i):
        return "ctx->acc = ps2_fadd(%s, %s);" % (f_f(i.fs), f_f(i.ft))

    def i_SUBA_S(self, i):
        return "ctx->acc = ps2_fsub(%s, %s);" % (f_f(i.fs), f_f(i.ft))

    def i_MULA_S(self, i):
        return "ctx->acc = ps2_fmul(%s, %s);" % (f_f(i.fs), f_f(i.ft))

    def i_MADD_S(self, i):
        return "%s = ps2_fadd(ctx->acc, ps2_fmul(%s, %s));" % (
            f_f(i.fd), f_f(i.fs), f_f(i.ft))

    def i_MSUB_S(self, i):
        return "%s = ps2_fsub(ctx->acc, ps2_fmul(%s, %s));" % (
            f_f(i.fd), f_f(i.fs), f_f(i.ft))

    def i_MADDA_S(self, i):
        return "ctx->acc = ps2_fadd(ctx->acc, ps2_fmul(%s, %s));" % (
            f_f(i.fs), f_f(i.ft))

    def i_MSUBA_S(self, i):
        return "ctx->acc = ps2_fsub(ctx->acc, ps2_fmul(%s, %s));" % (
            f_f(i.fs), f_f(i.ft))

    def i_CVT_W_S(self, i):
        return "%s = (u32)ps2_cvt_w_s(%s);" % (f_u(i.fd), f_f(i.fs))

    def i_CVT_S_W(self, i):
        return "%s = (float)(s32)%s;" % (f_f(i.fd), f_u(i.fs))

    def i_C_F_S(self, i):
        return "%s = 0;" % FCC

    def i_C_EQ_S(self, i):
        return "%s = (%s == %s);" % (FCC, f_f(i.fs), f_f(i.ft))

    def i_C_LT_S(self, i):
        return "%s = (%s < %s);" % (FCC, f_f(i.fs), f_f(i.ft))

    def i_C_LE_S(self, i):
        return "%s = (%s <= %s);" % (FCC, f_f(i.fs), f_f(i.ft))

    def i_QMFC2(self, i):
        return wrq(i.rt, "*(const ps2_reg128 *)&ctx->vu0.vf[%d]" % i.fs)

    def i_QMTC2(self, i):
        return "*(ps2_reg128 *)&ctx->vu0.vf[%d] = %s;" % (i.fs, rq(i.rt))

    def i_CFC2(self, i):
        return wrs32(i.rt, "ps2_vu0_cfc(ctx, %d)" % i.fs)

    def i_CTC2(self, i):
        return "ps2_vu0_ctc(ctx, %d, %s);" % (i.fs, ru32(i.rt))

    def i_VCALLMS(self, i):
        return "ps2_vu0_callms(ctx, 0x%Xu);" % (((i.word >> 6) & 0x7FFF) * 8)

    def i_VCALLMSR(self, i):
        return "ps2_vu0_callms(ctx, ctx->vu0.cmsar * 8);"

    def i_VNOP(self, i):
        return None

    def i_VWAITQ(self, i):
        return None

    def _delay(self, fn, ins, out, ind):
        ds_addr = ins.addr + 4
        ds = self.p.insns.get(ds_addr)
        if ds is None:
            raise ValueError("missing delay slot at %08X" % ds_addr)
        if ds.has_delay_slot:
            raise ValueError("branch in delay slot at %08X (fn %08X)"
                             % (ds_addr, fn.entry))
        self.emit_insn(ds, out, ind)
        self.flush_slots(out, ind)

    def _target_call(self, fn, target, out, ind, tail):
        p = self.p
        if target in p.functions:
            if tail:
                self.w(out, "{ PS2_TAIL return %s(ctx); }" % fname(target), ind)
            else:
                self.w(out, "%s(ctx);" % fname(target), ind)
            return
        if tail:
            self.w(out, "{ PS2_TAIL return ps2_dispatch(ctx, 0x%08Xu); }" % target, ind)
        else:
            self.w(out, "ps2_dispatch(ctx, 0x%08Xu);" % target, ind)

    def emit_transfer(self, fn, ins, out, ind=1):
        self.ee_slots += 1
        self.flush_slots(out, ind)
        p = self.p
        own = fn.own
        a = ins.addr
        cat = ins.cat

        if cat == CAT_JUMP and ins.name == "J":
            t = ins.target
            self._delay(fn, ins, out, ind)
            if t in own:
                self.w(out, self._back_edge(a, t) + "goto %s;" % label(t), ind)
            else:
                self._target_call(fn, t, out, ind, tail=True)
            return

        if cat == CAT_JUMP and ins.name == "JAL":
            w = wr64(31, "0x%08Xll" % ((a + 8) & 0xFFFFFFFF))
            self.w(out, w, ind)
            self._delay(fn, ins, out, ind)
            self._target_call(fn, ins.target, out, ind, tail=False)
            return

        if cat == CAT_JUMPR and ins.name == "JR":
            if ins.rs == 31:
                self._delay(fn, ins, out, ind)
                self.w(out, "return;", ind)
                return
            sw = p.switches.get(a)
            self.w(out, "{ u32 _t = %s;" % ru32(ins.rs), ind)
            self._delay(fn, ins, out, ind + 1)
            if sw:
                self.w(out, "switch (_t) {", ind + 1)
                for t in sw:
                    if t in own:
                        self.w(out, "case 0x%08Xu: goto %s;" % (t, label(t)),
                               ind + 2)
                    else:
                        self.w(out, "case 0x%08Xu:" % t, ind + 2)
                        self._target_call(fn, t, out, ind + 3, tail=True)
                self.w(out, "default: break;", ind + 2)
                self.w(out, "}", ind + 1)
            self.w(out, "ctx->pc = 0x%08Xu;" % a, ind + 1)
            self.w(out, "{ PS2_TAIL return ps2_dispatch(ctx, _t); } }", ind + 1)
            return

        if cat == CAT_JUMPR and ins.name == "JALR":
            self.w(out, "{ u32 _t = %s;" % ru32(ins.rs), ind)
            w = wr64(ins.rd, "0x%08Xll" % ((a + 8) & 0xFFFFFFFF))
            if w:
                self.w(out, w, ind + 1)
            self._delay(fn, ins, out, ind + 1)
            self.w(out, "ps2_dispatch(ctx, _t); }", ind + 1)
            return

        cond = self._branch_cond(ins)
        t = ins.btarget
        likely = (cat == CAT_BRANCH_LIKELY)
        if ins.writes_ra:
            pre = wr64(31, "0x%08Xll" % ((a + 8) & 0xFFFFFFFF))
        else:
            pre = None

        if likely:
            self.w(out, "if (%s) {" % cond, ind)
            if pre:
                self.w(out, pre, ind + 1)
            self._delay(fn, ins, out, ind + 1)
            if t in own:
                self.w(out, self._back_edge(a, t) + "goto %s;" % label(t),
                       ind + 1)
            else:
                self._target_call(fn, t, out, ind + 1, tail=True)
            self.w(out, "}", ind)
        else:
            self.w(out, "{ int _c = (%s);" % cond, ind)
            if pre:
                self.w(out, pre, ind + 1)
            self._delay(fn, ins, out, ind + 1)
            if t in own:
                if self._back_edge(a, t):
                    self.w(out, "if (_c) { PS2_LOOP(); goto %s; } }"
                           % label(t), ind + 1)
                else:
                    self.w(out, "if (_c) goto %s; }" % label(t), ind + 1)
            else:
                self.w(out, "if (_c) {", ind + 1)
                self._target_call(fn, t, out, ind + 2, tail=True)
                self.w(out, "} }", ind + 1)

    @staticmethod
    def _back_edge(a, t):
        return "PS2_LOOP(); " if t <= a else ""

    def _branch_cond(self, i):
        n = i.name
        if n in ("BEQ", "BEQL"):
            if i.rs == 0 and i.rt == 0:
                return "1"
            return "%s == %s" % (ru64(i.rs), ru64(i.rt))
        if n in ("BNE", "BNEL"):
            return "%s != %s" % (ru64(i.rs), ru64(i.rt))
        if n in ("BLEZ", "BLEZL"):
            return "(s64)(%s) <= 0" % rs64(i.rs)
        if n in ("BGTZ", "BGTZL"):
            return "(s64)(%s) > 0" % rs64(i.rs)
        if n in ("BLTZ", "BLTZL", "BLTZAL", "BLTZALL"):
            return "(s64)(%s) < 0" % rs64(i.rs)
        if n in ("BGEZ", "BGEZL", "BGEZAL", "BGEZALL"):
            return "(s64)(%s) >= 0" % rs64(i.rs)
        if n in ("BC1T", "BC1TL"):
            return "%s" % FCC
        if n in ("BC1F", "BC1FL"):
            return "!%s" % FCC
        if n in ("BC2T", "BC2TL"):
            return "ctx->vu0.cc"
        if n in ("BC2F", "BC2FL"):
            return "!ctx->vu0.cc"
        if n in ("BC0T", "BC0TL"):
            return "1"
        if n in ("BC0F", "BC0FL"):
            return "0"
        self.warn("unknown branch %s at %08x" % (n, i.addr))
        return "0"

    def emit_function(self, fn):
        self.ee_slots = 0
        p = self.p
        out = []
        fn.own = set(fn.addrs)
        nm = self.sym(fn.entry)

        ov = self.overrides.get(fn.entry)
        if ov:
            self.overrides_used[fn.entry] = ov
            out.append("/* %08X  %s  -- HLE override -> %s() */"
                       % (fn.entry, nm or "", ov))
            out.append("void %s(ps2_ctx *ctx) {" % fname(fn.entry))
            out.append("    PS2_ENTER(0x%08Xu);" % fn.entry)
            out.append("    %s(ctx);" % ov)
            out.append("}")
            return "\n".join(out) + "\n"

        targets = set()
        for a in fn.addrs:
            ins = p.insns[a]
            if ins.cat in (CAT_BRANCH, CAT_BRANCH_LIKELY):
                targets.add(ins.btarget)
            elif ins.name == "J":
                targets.add(ins.target)
            targets.update(p.switches.get(a, ()))
        ds_entries = targets & set(fn.delay_slots)
        for a in ds_entries:
            if a not in p.insns or p.insns[a].has_delay_slot or a + 4 not in fn.own:
                raise ValueError("unsupported delay-slot entry at %08X" % a)
            fn.own.add(a)
            fn.labels.add(a + 4)
        if not hasattr(self, "all_delay_slots"):
            self.all_delay_slots = {a for f in p.functions.values() for a in f.delay_slots}
        all_delay = self.all_delay_slots
        for a in targets & all_delay:
            if a not in fn.own and a not in p.functions:
                raise ValueError("cross-function delay-slot entry at %08X" % a)

        hdr = "/* %08X  %s  (%d insns" % (fn.entry, nm or "", len(fn.addrs))
        hdr += ") */"
        out.append(hdr)
        out.append("void %s(ps2_ctx *ctx) {" % fname(fn.entry))
        out.append("    PS2_ENTER(0x%08Xu);" % fn.entry)
        hk = self.hooks.get(fn.entry)
        if hk:
            out.append("    %s(ctx);   /* entry hook */" % hk)

        addrs = fn.addrs
        late_entry = bool(addrs and addrs[0] != fn.entry)
        if late_entry:
            if fn.entry not in fn.own:
                raise ValueError("function %08X does not own its entry" % fn.entry)
            self.w(out, "goto %s;" % label(fn.entry))
        prev = None
        for idx, a in enumerate(addrs):
            ins = p.insns[a]
            if a in fn.labels or (late_entry and a == fn.entry):
                self.flush_slots(out)
                out.append("%s:;" % label(a))
            if self.comments:
                out.append("    /* %08x %08x %s */" % (a, ins.word, ins.name))

            if ins.cat in (CAT_BRANCH, CAT_BRANCH_LIKELY, CAT_JUMP, CAT_JUMPR):
                self.emit_transfer(fn, ins, out, 1)
                nat = a + 8
            else:
                self.emit_insn(ins, out, 1)
                nat = a + 4

            can_ft = ins.cat not in (CAT_JUMP, CAT_JUMPR) or ins.name in (
                "JAL", "JALR")
            if ins.cat == CAT_JUMP and ins.name == "J":
                can_ft = False
            if ins.cat == CAT_JUMPR and ins.name == "JR":
                can_ft = False
            nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
            if can_ft and nxt != nat:
                self.flush_slots(out)
                if p.in_text(nat):
                    self._target_call(fn, nat, out, 1, tail=True)
                else:
                    self.w(out, "return;", 1)
        self.flush_slots(out)
        if ds_entries:
            self.w(out, "return;")
        for a in sorted(ds_entries):
            out.append("%s:;" % label(a))
            self.emit_insn(p.insns[a], out, 1)
            self.flush_slots(out)
            self.w(out, "goto %s;" % label(a + 4))
        out.append("}")
        return "\n".join(out) + "\n"

    def emit_all(self, outdir, per_file=250):
        p = self.p
        os.makedirs(outdir, exist_ok=True)
        entries = sorted(p.functions)
        chunks = [entries[i:i + per_file]
                  for i in range(0, len(entries), per_file)]

        with open(os.path.join(outdir, "ps2_funcs.h"), "w") as fp:
            fp.write("/* generated by ps2recomp -- do not edit */\n")
            fp.write("#ifndef PS2_FUNCS_H\n#define PS2_FUNCS_H\n")
            fp.write('#include "ps2_runtime.h"\n\n')
            fp.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
            if self.overrides or self.hooks:
                fp.write("/* native HLE handlers substituted for guest code, "
                         "and entry hooks */\n")
                for h in sorted(set(self.overrides.values())
                                | set(self.hooks.values())):
                    fp.write("void %s(ps2_ctx *ctx);\n" % h)
                fp.write("\n")
            for e in entries:
                nm = self.sym(e)
                if nm:
                    fp.write("void %s(ps2_ctx *ctx);  /* %s */\n"
                             % (fname(e), nm))
                else:
                    fp.write("void %s(ps2_ctx *ctx);\n" % fname(e))
            fp.write("#ifdef __cplusplus\n}\n#endif\n")
            fp.write("#endif\n")

        nfiles = 0
        for ci, chunk in enumerate(chunks):
            path = os.path.join(outdir, "ps2_code_%04d.c" % ci)
            with open(path, "w") as fp:
                fp.write("/* generated by ps2recomp -- do not edit */\n")
                fp.write('#include "ps2_runtime.h"\n')
                fp.write('#include "ps2_funcs.h"\n\n')
                for e in chunk:
                    fp.write(self.emit_function(p.functions[e]))
                    fp.write("\n")
            nfiles += 1

        with open(os.path.join(outdir, "ps2_func_table.c"), "w") as fp:
            fp.write("/* generated by ps2recomp -- do not edit */\n")
            fp.write('#include "ps2_runtime.h"\n#include "ps2_funcs.h"\n\n')
            fp.write("const ps2_func_entry ps2_func_table[] = {\n")
            for e in entries:
                fp.write("    { 0x%08Xu, %s },\n" % (e, fname(e)))
            fp.write("};\n")
            fp.write("const unsigned ps2_func_count = %d;\n" % len(entries))
            fp.write("const u32 ps2_entry_point = 0x%08Xu;\n" % p.elf.entry)

        named = sorted((a, n) for a, n in self.symbols.items() if p.in_text(a))
        with open(os.path.join(outdir, "ps2_symbols.c"), "w") as fp:
            fp.write("/* generated by ps2recomp -- do not edit */\n")
            fp.write('#include "ps2_runtime.h"\n\n')
            fp.write("const ps2_symbol ps2_symbols[] = {\n")
            for a, n in named:
                fp.write('    { 0x%08Xu, "%s" },\n' % (a, n))
            fp.write("};\n")
            fp.write("const unsigned ps2_symbol_count = %d;\n" % len(named))
        return nfiles, len(entries)


def _install_vu_handlers():
    bc_index = {"x": 0, "y": 1, "z": 2, "w": 3}

    def mk_bc(base):
        def h(self, i):
            b = bc_index[i.name[-1]]
            return "ps2_vu0_%s(ctx, 0x%X, %d, %d, %d, %d);" % (
                base, i.dest, i.fd, i.fs, i.ft, b)
        return h

    def mk_std(base):
        def h(self, i):
            return "ps2_vu0_%s(ctx, 0x%X, %d, %d, %d);" % (
                base, i.dest, i.fd, i.fs, i.ft)
        return h

    def mk_qi(base, src):
        def h(self, i):
            return "ps2_vu0_%s_%s(ctx, 0x%X, %d, %d);" % (
                base, src, i.dest, i.fd, i.fs)
        return h

    def mk_acc_bc(base):
        def h(self, i):
            b = bc_index[i.name[-1]]
            return "ps2_vu0_%s(ctx, 0x%X, %d, %d, %d);" % (
                base, i.dest, i.fs, i.ft, b)
        return h

    def mk_acc(base):
        def h(self, i):
            return "ps2_vu0_%s(ctx, 0x%X, %d, %d);" % (base, i.dest, i.fs, i.ft)
        return h

    def mk_acc_qi(base, src):
        def h(self, i):
            return "ps2_vu0_%s_%s(ctx, 0x%X, %d);" % (base, src, i.dest, i.fs)
        return h

    def mk_conv(base, bits):
        def h(self, i):
            return "ps2_vu0_%s(ctx, 0x%X, %d, %d, %d);" % (
                base, i.dest, i.ft, i.fs, bits)
        return h

    reg = {}
    for op in ("add", "sub", "mul", "max", "mini", "madd", "msub"):
        NAME = {"add": "VADD", "sub": "VSUB", "mul": "VMUL", "max": "VMAX",
                "mini": "VMINI", "madd": "VMADD", "msub": "VMSUB"}[op]
        for c in "xyzw":
            reg[NAME + c] = mk_bc(op + "_bc")
        reg[NAME] = mk_std(op)
        for s in ("q", "i"):
            reg[NAME + s] = mk_qi(op, s)
    reg["VOPMSUB"] = mk_std("opmsub")
    reg["VMAXi"] = mk_qi("max", "i")
    reg["VMINIi"] = mk_qi("mini", "i")

    for op in ("adda", "suba", "mula", "madda", "msuba"):
        NAME = "V" + op.upper()
        for c in "xyzw":
            reg[NAME + c] = mk_acc_bc(op + "_bc")
        reg[NAME] = mk_acc(op)
        for s in ("q", "i"):
            reg[NAME + s] = mk_acc_qi(op, s)
    reg["VOPMULA"] = mk_acc("opmula")

    for bits in (0, 4, 12, 15):
        reg["VITOF%d" % bits] = mk_conv("itof", bits)
        reg["VFTOI%d" % bits] = mk_conv("ftoi", bits)

    def h_abs(self, i):
        return "ps2_vu0_abs(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_move(self, i):
        return "ps2_vu0_move(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_mr32(self, i):
        return "ps2_vu0_mr32(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_clip(self, i):
        return "ps2_vu0_clip(ctx, %d, %d);" % (i.fs, i.ft)

    def h_div(self, i):
        return "ps2_vu0_div(ctx, %d, %d, %d, %d);" % (
            i.fs, (i.word >> 21) & 3, i.ft, (i.word >> 23) & 3)

    def h_sqrt(self, i):
        return "ps2_vu0_sqrt(ctx, %d, %d);" % (i.ft, (i.word >> 23) & 3)

    def h_rsqrt(self, i):
        return "ps2_vu0_rsqrt(ctx, %d, %d, %d, %d);" % (
            i.fs, (i.word >> 21) & 3, i.ft, (i.word >> 23) & 3)

    def h_iadd(self, i):
        return "ctx->vu0.vi[%d] = (u16)(ctx->vu0.vi[%d] + ctx->vu0.vi[%d]);" % (
            i.fd, i.fs, i.ft)

    def h_isub(self, i):
        return "ctx->vu0.vi[%d] = (u16)(ctx->vu0.vi[%d] - ctx->vu0.vi[%d]);" % (
            i.fd, i.fs, i.ft)

    def h_iaddi(self, i):
        imm = (i.word >> 6) & 0x1F
        if imm & 0x10:
            imm -= 0x20
        return "ctx->vu0.vi[%d] = (u16)(ctx->vu0.vi[%d] + (%d));" % (
            i.ft, i.fs, imm)

    def h_iand(self, i):
        return "ctx->vu0.vi[%d] = ctx->vu0.vi[%d] & ctx->vu0.vi[%d];" % (
            i.fd, i.fs, i.ft)

    def h_ior(self, i):
        return "ctx->vu0.vi[%d] = ctx->vu0.vi[%d] | ctx->vu0.vi[%d];" % (
            i.fd, i.fs, i.ft)

    def h_mtir(self, i):
        return "ctx->vu0.vi[%d] = (u16)(ctx->vu0.vf[%d].u[%d] & 0xFFFFu);" % (
            i.ft, i.fs, (i.word >> 21) & 3)

    def h_mfir(self, i):
        return "ps2_vu0_mfir(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_ilwr(self, i):
        return "ctx->vu0.vi[%d] = ps2_vu0_ilwr(ctx, 0x%X, %d);" % (
            i.ft, i.dest, i.fs)

    def h_iswr(self, i):
        return "ps2_vu0_iswr(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_lqi(self, i):
        return "ps2_vu0_lqi(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_lqd(self, i):
        return "ps2_vu0_lqd(ctx, 0x%X, %d, %d);" % (i.dest, i.ft, i.fs)

    def h_sqi(self, i):
        return "ps2_vu0_sqi(ctx, 0x%X, %d, %d);" % (i.dest, i.fs, i.ft)

    def h_sqd(self, i):
        return "ps2_vu0_sqd(ctx, 0x%X, %d, %d);" % (i.dest, i.fs, i.ft)

    def h_rnext(self, i):
        return "ps2_vu0_rnext(ctx, 0x%X, %d);" % (i.dest, i.ft)

    def h_rget(self, i):
        return "ps2_vu0_rget(ctx, 0x%X, %d);" % (i.dest, i.ft)

    def h_rinit(self, i):
        return "ps2_vu0_rinit(ctx, %d, %d);" % (i.fs, (i.word >> 21) & 3)

    def h_rxor(self, i):
        return "ps2_vu0_rxor(ctx, %d, %d);" % (i.fs, (i.word >> 21) & 3)

    reg.update({
        "VABS": h_abs, "VMOVE": h_move, "VMR32": h_mr32, "VCLIPw": h_clip,
        "VDIV": h_div, "VSQRT": h_sqrt, "VRSQRT": h_rsqrt,
        "VIADD": h_iadd, "VISUB": h_isub, "VIADDI": h_iaddi,
        "VIAND": h_iand, "VIOR": h_ior,
        "VMTIR": h_mtir, "VMFIR": h_mfir, "VILWR": h_ilwr, "VISWR": h_iswr,
        "VLQI": h_lqi, "VLQD": h_lqd, "VSQI": h_sqi, "VSQD": h_sqd,
        "VRNEXT": h_rnext, "VRGET": h_rget, "VRINIT": h_rinit, "VRXOR": h_rxor,
    })

    for name, fnh in reg.items():
        setattr(Emitter, "i_" + name.replace(".", "_"), fnh)


_install_vu_handlers()


def _install_mmi_handlers():
    simple = [
        "PADDW", "PSUBW", "PCGTW", "PMAXW", "PADDH", "PSUBH", "PCGTH",
        "PMAXH", "PADDB", "PSUBB", "PCGTB", "PADDSW", "PSUBSW", "PEXTLW",
        "PPACW", "PADDSH", "PSUBSH", "PEXTLH", "PPACH", "PADDSB", "PSUBSB",
        "PEXTLB", "PPACB", "PEXT5", "PPAC5", "PABSW", "PCEQW", "PMINW",
        "PADSBH", "PABSH", "PCEQH", "PMINH", "PCEQB", "PADDUW", "PSUBUW",
        "PEXTUW", "PADDUH", "PSUBUH", "PEXTUH", "PADDUB", "PSUBUB", "PEXTUB",
        "QFSRV", "PMADDW", "PSLLVW", "PSRLVW", "PMSUBW", "PINTH", "PMULTW",
        "PDIVW", "PCPYLD", "PMADDH", "PHMADH", "PAND", "PXOR", "PMSUBH",
        "PHMSBH", "PEXEH", "PREVH", "PMULTH", "PDIVBW", "PEXEW", "PROT3W",
        "PMADDUW", "PSRAVW", "PINTEH", "PMULTUW", "PDIVUW", "PCPYUD", "POR",
        "PNOR", "PEXCH", "PCPYH", "PEXCW",
    ]

    def mk3(name):
        low = name.lower()

        def h(self, i):
            return "ps2_%s(ctx, %d, %d, %d);" % (low, i.rd, i.rs, i.rt)
        return h

    for nm in simple:
        setattr(Emitter, "i_" + nm, mk3(nm))

    def mk_shift(name):
        low = name.lower()

        def h(self, i):
            return "ps2_%s(ctx, %d, %d, %d);" % (low, i.rd, i.rt, i.sa)
        return h

    for nm in ("PSLLH", "PSRLH", "PSRAH", "PSLLW", "PSRLW", "PSRAW"):
        setattr(Emitter, "i_" + nm, mk_shift(nm))

    def h_pmfhi(self, i):
        return "ps2_pmfhi(ctx, %d);" % i.rd

    def h_pmflo(self, i):
        return "ps2_pmflo(ctx, %d);" % i.rd

    def h_pmthi(self, i):
        return "ps2_pmthi(ctx, %d);" % i.rs

    def h_pmtlo(self, i):
        return "ps2_pmtlo(ctx, %d);" % i.rs

    def h_pmfhl(self, i):
        return "ps2_pmfhl(ctx, %d, %d);" % (i.rd, i.sa)

    def h_pmthl(self, i):
        return "ps2_pmthl(ctx, %d, %d);" % (i.rs, i.sa)

    Emitter.i_PMFHI = h_pmfhi
    Emitter.i_PMFLO = h_pmflo
    Emitter.i_PMTHI = h_pmthi
    Emitter.i_PMTLO = h_pmtlo
    Emitter.i_PMFHL = h_pmfhl
    Emitter.i_PMTHL = h_pmthl


_install_mmi_handlers()
