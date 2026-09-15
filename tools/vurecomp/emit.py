from vurecomp import vudec
from vurecomp.cfg import build_cfg

_UOP = {
    "ADD": ("vu_add4", False), "SUB": ("vu_sub4", False),
    "MUL": ("vu_mul4", False), "MAX": ("vu_max4", False),
    "MINI": ("vu_min4", False),
    "MADD": ("vu_madd4", True), "MSUB": ("vu_msub4", True),
}
_NO_FLAGS = {"MAX", "MINI"}


def _split_upper(name):
    base, acc, b = name, False, "ft"
    if base.endswith(("x", "y", "z", "w")) and base[:-1] in (
            "ADD", "SUB", "MUL", "MADD", "MSUB", "MAX", "MINI",
            "ADDA", "SUBA", "MULA", "MADDA", "MSUBA"):
        b = "bc"
        base = base[:-1]
    elif base.endswith("q"):
        b, base = "q", base[:-1]
    elif base.endswith("i"):
        b, base = "i", base[:-1]
    if base.endswith("A") and base[:-1] in _UOP:
        acc, base = True, base[:-1]
    return base, acc, b


def emit_upper(pair, out):
    u = pair.upper
    n = u.name
    if n == "NOP":
        return
    if n.startswith("ITOF") or n.startswith("FTOI"):
        sh = int(n[4:])
        fn = "vu_itof4" if n.startswith("ITOF") else "vu_ftoi4"
        out.append("    vu_wr(vu, %d, 0x%X, %s(&vu->vf[%d], %d));"
                   % (u.ft, u.dest, fn, u.fs, sh))
        return
    if n == "ABS":
        out.append("    vu_wr(vu, %d, 0x%X, vu_abs4(vu_ld(&vu->vf[%d])));"
                   % (u.ft, u.dest, u.fs))
        return

    base, acc, bsel = _split_upper(n)
    if base not in _UOP:
        out.append("    vu_upper(vu, 0x%08Xu, fslot);" % (pair.up_word & 0x07FFFFFF))
        return

    helper, wants_acc = _UOP[base]
    if bsel == "bc":
        b = "vu_set1(vu->vf[%d].f[%d])" % (u.ft, u.bc)
    elif bsel == "q":
        b = "vu_set1(vu->q)"
    elif bsel == "i":
        b = "vu_set1(vu->i)"
    else:
        b = "vu_ld(&vu->vf[%d])" % u.ft
    a = "vu_ld(&vu->vf[%d])" % u.fs
    expr = ("%s(vu_ld(&vu->acc), %s, %s)" % (helper, a, b)) if wants_acc \
        else ("%s(%s, %s)" % (helper, a, b))

    out.append("    { vu_f4 r = %s;" % expr)
    if base not in _NO_FLAGS:
        out.append("      vu_set_flags(vu, 0x%X, r, fslot);" % u.dest)
    if acc:
        out.append("      vu_blend(&vu->acc, 0x%X, r); }" % u.dest)
    else:
        out.append("      vu_wr(vu, %d, 0x%X, r); }" % (u.fd, u.dest))


_DIRECT = {
    "MOVE", "MR32", "LQ", "SQ", "LQI", "SQI", "LQD", "SQD",
    "ILW", "ISW", "ILWR", "ISWR",
    "IADD", "ISUB", "IADDI", "IAND", "IOR", "IADDIU", "ISUBIU",
    "DIV", "SQRT", "RSQRT", "WAITQ",
    "FMAND", "FMOR", "MTIR", "MFIR", "XTOP", "XITOP", "XGKICK",
}


def _lanes(dest):
    return [i for i in range(4) if (dest >> (3 - i)) & 1]


def emit_lower(pair, out):
    l = pair.lower
    if l is None:
        return
    n = l.name
    if l.kind == "nop":
        return
    if n not in _DIRECT:
        out.append("    (void)vu_lower(vu, 0x%08Xu, 0x%04Xu, guard);"
                   % (pair.lo_word, pair.pc + 8))
        return

    it, is_, dest = l.ft, l.fs, l.dest
    itm, ism = it & 15, is_ & 15

    if n in ("MOVE", "MR32"):
        if dest == 0:
            return
        src = ([0, 1, 2, 3] if n == "MOVE" else [1, 2, 3, 0])
        out.append("    { float r[4];")
        for i in range(4):
            out.append("      r[%d] = vu->vf[%d].f[%d];" % (i, is_, src[i]))
        out.append("      vu_store(vu, %d, 0x%X, r); }" % (it, dest))
        return

    if n == "LQ":
        out.append("    { float r[4]; u32 a = ((u32)vu->vi[%d] + %du) * 16u;"
                   % (ism, l.imm & 0xFFFFFFFF))
        out.append("      memcpy(r, mem + (a & memmask16), 16);")
        out.append("      vu_store(vu, %d, 0x%X, r); }" % (it, dest))
        return
    if n in ("LQI", "LQD"):
        if n == "LQD":
            out.append("    if (%d) vu->vi[%d] = (u16)(vu->vi[%d] - 1u);"
                       % (ism, ism, ism))
        out.append("    { float r[4]; u32 a = ((u32)vu->vi[%d] * 16u) & memmask16;"
                   % ism)
        out.append("      memcpy(r, mem + a, 16);")
        out.append("      vu_store(vu, %d, 0x%X, r); }" % (it, dest))
        if n == "LQI":
            out.append("    if (%d) vu->vi[%d] = (u16)(vu->vi[%d] + 1u);"
                       % (ism, ism, ism))
        return
    if n == "SQ":
        out.append("    { u32 off = (((u32)vu->vi[%d] + %du) * 16u) & memmask16;"
                   % (itm, l.imm & 0xFFFF))
        for i in _lanes(dest):
            out.append("      memcpy(mem + off + %d, &vu->vf[%d].f[%d], 4);"
                       % (i * 4, is_, i))
        out.append("    }")
        return
    if n in ("SQI", "SQD"):
        if n == "SQD":
            out.append("    if (%d) vu->vi[%d] = (u16)(vu->vi[%d] - 1u);"
                       % (itm, itm, itm))
        out.append("    { u32 off = ((u32)vu->vi[%d] * 16u) & memmask16;" % itm)
        for i in _lanes(dest):
            out.append("      memcpy(mem + off + %d, &vu->vf[%d].f[%d], 4);"
                       % (i * 4, is_, i))
        out.append("    }")
        if n == "SQI":
            out.append("    if (%d) vu->vi[%d] = (u16)(vu->vi[%d] + 1u);"
                       % (itm, itm, itm))
        return
    if n in ("ILW", "ILWR"):
        lane = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3
        base = ("((u32)vu->vi[%d] + %du) * 16u" % (ism, l.imm & 0xFFFFFFFF)) \
            if n == "ILW" else ("(u32)vu->vi[%d] * 16u" % ism)
        out.append("    { u16 v; u32 a = %s;" % base)
        out.append("      memcpy(&v, mem + ((a + %du) & memmask4), 2);" % (lane * 4))
        if itm:
            out.append("      vu->vi[%d] = v; }" % itm)
        else:
            out.append("      (void)v; }")
        return
    if n in ("ISW", "ISWR"):
        base = ("((u32)vu->vi[%d] + %du) * 16u" % (ism, l.imm & 0xFFFFFFFF)) \
            if n == "ISW" else ("(u32)vu->vi[%d] * 16u" % ism)
        out.append("    { u32 v = vu->vi[%d], a = %s;" % (itm, base))
        for i in _lanes(dest):
            out.append("      memcpy(mem + ((a + %du) & memmask4), &v, 4);" % (i * 4))
        out.append("    }")
        return

    if n in ("IADD", "ISUB", "IAND", "IOR"):
        oper = {"IADD": "+", "ISUB": "-", "IAND": "&", "IOR": "|"}[n]
        if l.fd & 15:
            out.append("    vu->vi[%d] = (u16)(vu->vi[%d] %s vu->vi[%d]);"
                       % (l.fd & 15, ism, oper, itm))
        return
    if n == "IADDI":
        if itm:
            out.append("    vu->vi[%d] = (u16)(vu->vi[%d] + (u16)%d);"
                       % (itm, ism, l.imm))
        return
    if n in ("IADDIU", "ISUBIU"):
        oper = "+" if n == "IADDIU" else "-"
        if itm:
            out.append("    vu->vi[%d] = (u16)(vu->vi[%d] %s (u16)%du);"
                       % (itm, ism, oper, l.imm))
        return

    if n == "DIV":
        out.append("    vu_fdiv_issue(vu, guard, 7, ps2_fdiv(vu->vf[%d].f[%d], "
                   "vu->vf[%d].f[%d]));" % (is_, l.fsf, it, l.ftf))
        return
    if n == "SQRT":
        out.append("    vu_fdiv_issue(vu, guard, 7, ps2_fsqrt(vu->vf[%d].f[%d]));"
                   % (it, l.ftf))
        return
    if n == "RSQRT":
        out.append("    vu_fdiv_issue(vu, guard, 13, ps2_frsqrt(vu->vf[%d].f[%d], "
                   "vu->vf[%d].f[%d]));" % (is_, l.fsf, it, l.ftf))
        return
    if n == "WAITQ":
        out.append("    vu_q_commit(vu);")
        return

    if n in ("FMAND", "FMOR"):
        oper = "&" if n == "FMAND" else "|"
        if itm:
            out.append("    vu->vi[%d] = (u16)(vu->mac %s vu->vi[%d]);"
                       % (itm, oper, ism))
        return
    if n == "MTIR":
        if itm:
            out.append("    vu->vi[%d] = (u16)(vu->vf[%d].u[%d] & 0xFFFFu);"
                       % (itm, is_, l.fsf))
        return
    if n == "MFIR":
        out.append("    { float r[4]; s32 v = (s32)(s16)vu->vi[%d]; int k;" % ism)
        out.append("      for (k = 0; k < 4; k++) memcpy(&r[k], &v, 4);")
        out.append("      vu_store(vu, %d, 0x%X, r); }" % (it, dest))
        return
    if n in ("XTOP", "XITOP"):
        if itm:
            out.append("    vu->vi[%d] = (u16)vu->%s;"
                       % (itm, "top" if n == "XTOP" else "itop"))
        return
    if n == "XGKICK":
        out.append("    vu_do_xgkick(vu, (u32)vu->vi[%d]);" % ism)
        return

    raise AssertionError("unhandled direct lower op %s" % n)


class Unsupported(Exception):
    pass


def _check(code, cfg):
    for blk in cfg.blocks.values():
        for idx, pc in enumerate(blk.pairs):
            pair = code[pc]
            l = pair.lower
            if l is None:
                continue
            if l.kind in ("branch", "jump") and idx and \
                    code[blk.pairs[idx - 1]].lower is not None and \
                    code[blk.pairs[idx - 1]].lower.kind in ("branch", "jump"):
                raise Unsupported("branch in a delay slot at %04X" % pc)
            if l.kind in ("branch", "jump") and pc + 8 in code and \
                    code[pc + 8].ebit:
                raise Unsupported("E bit in a branch delay slot at %04X" % pc)
            if l.kind in ("branch", "jump") and pair.ebit:
                raise Unsupported("E bit on a branch at %04X" % pc)


def emit_program(crc, image, entries, code_lo, code_hi):
    code = vudec.decode_image(image, code_lo, code_hi)
    pcmask = len(image) - 8
    cfg = build_cfg(code, entries)
    if not cfg.blocks:
        raise Unsupported("no reachable blocks")
    _check(code, cfg)
    cfg.pcmask = pcmask

    fn = "vu1_prog_%s" % crc
    out = []
    out.append("/* %s -- %d blocks, %d pairs, entries %s */"
               % (crc, len(cfg.blocks), cfg.reachable_pairs,
                  " ".join("%04X" % e for e in cfg.entries)))

    reach = sorted({pc for b in cfg.blocks.values() for pc in b.pairs})
    out.append("static const unsigned short vu1_sig_pc_%s[] = {" % crc)
    for i in range(0, len(reach), 12):
        out.append("    " + " ".join("0x%04X," % p for p in reach[i:i + 12]))
    out.append("};")
    out.append("static const unsigned int vu1_sig_w_%s[] = {" % crc)
    for i in range(0, len(reach), 6):
        row = []
        for p in reach[i:i + 6]:
            row.append("0x%08X,0x%08X," % (code[p].lo_word, code[p].up_word))
        out.append("    " + "".join(row))
    out.append("};")
    out.append("static int %s(ps2_vu *vu, u32 start, u32 limit," % fn)
    out.append("                     u32 *pc_io, u32 *guard_io, u32 *fslot_io) {")
    out.append("    u32 guard = 0, fslot = 0, pc = start, jt = 0;")
    out.append("    int taken = 0;")
    out.append("    u8 *mem = vu->mem;")
    out.append("    (void)jt; (void)taken;")
    out.append("    const u32 memmask16 = vu->mem_size - 16u;")
    out.append("    const u32 memmask4  = vu->mem_size - 4u;")
    out.append("    (void)memmask4;")
    out.append("    switch (start) {")
    for b in sorted(cfg.blocks):
        out.append("    case 0x%04Xu: goto B_%04X;" % (b, b))
    out.append("    default: return 0;          /* not an entry we lifted */")
    out.append("    }")

    for start in sorted(cfg.blocks):
        blk = cfg.blocks[start]
        out.append("")
        out.append("B_%04X:" % start)
        _emit_block(code, cfg, blk, out)

    out.append("")
    out.append("vu1_done:")
    out.append("    *pc_io = pc; *guard_io = guard; *fslot_io = fslot;")
    out.append("    return 1;")
    out.append("vu1_runaway:")
    out.append("    *pc_io = pc; *guard_io = guard; *fslot_io = fslot;")
    out.append("    return 3; /* watchdog, not an E-bit stop */")
    out.append("}")
    return "\n".join(out), cfg, reach, [(code[p].lo_word, code[p].up_word)
                                        for p in reach], sorted(cfg.blocks)


def _step(out, pc):
    out.append("    pc = 0x%04Xu;" % pc)
    out.append("    if (PS2_UNLIKELY(guard >= limit)) goto vu1_runaway;")
    out.append("    guard++;")
    out.append("    fslot = (guard - 1u) & 3u;")
    out.append("    if (vu->fpipe[fslot].valid) {")
    out.append("        vu->mac = vu->fpipe[fslot].mac;")
    out.append("        vu->status = vu->fpipe[fslot].status;")
    out.append("        vu->fpipe[fslot].valid = 0;")
    out.append("    }")
    out.append("    if (PS2_UNLIKELY(vu->q_ready) && guard >= vu->q_ready) vu_q_commit(vu);")
    out.append("    if (PS2_UNLIKELY(vu->p_ready) && guard >= vu->p_ready) vu_p_commit(vu);")


def _emit_pair(code, pc, out):
    pair = code[pc]
    _step(out, pc)
    emit_lower(pair, out)
    emit_upper(pair, out)
    if pair.ibit:
        out.append("    { u32 t = 0x%08Xu; memcpy(&vu->i, &t, 4); }" % pair.lo_word)


def _emit_block(code, cfg, blk, out):
    pairs = blk.pairs
    term_idx = None
    for i, pc in enumerate(pairs):
        pair = code[pc]
        l = pair.lower
        if pair.ebit or (l is not None and l.kind in ("branch", "jump")):
            term_idx = i
            break

    if term_idx is None:
        for pc in pairs:
            _emit_pair(code, pc, out)
        nxt = (pairs[-1] + 8) & cfg.pcmask
        if blk.succs and blk.succs[0] in cfg.blocks:
            out.append("    goto B_%04X;" % blk.succs[0])
        elif nxt in cfg.blocks:
            out.append("    goto B_%04X;" % nxt)
        else:
            out.append("    pc = 0x%04Xu; *pc_io = pc; *guard_io = guard; "
                       "*fslot_io = fslot; return 2;" % nxt)
        return

    for pc in pairs[:term_idx]:
        _emit_pair(code, pc, out)

    tpc = pairs[term_idx]
    term = code[tpc]
    tl = term.lower

    cond = None
    if tl is not None and tl.kind == "branch":
        n, ism, itm = tl.name, tl.fs & 15, tl.ft & 15
        if n == "B":
            cond = "1"
        elif n == "BAL":
            cond = "1"
        else:
            cond = {
                "IBEQ": "vu->vi[%d] == vu->vi[%d]" % (itm, ism),
                "IBNE": "vu->vi[%d] != vu->vi[%d]" % (itm, ism),
                "IBLTZ": "(s16)vu->vi[%d] <  0" % ism,
                "IBGTZ": "(s16)vu->vi[%d] >  0" % ism,
                "IBLEZ": "(s16)vu->vi[%d] <= 0" % ism,
                "IBGEZ": "(s16)vu->vi[%d] >= 0" % ism,
            }[n]

    _step(out, tpc)
    if tl is not None and tl.kind == "branch" and cond != "1":
        out.append("    taken = (%s) ? 1 : 0;" % cond)
    if tl is not None and tl.name in ("BAL", "JALR") and (tl.ft & 15):
        out.append("    vu->vi[%d] = (u16)(%uu & 0xFFFFu);"
                   % (tl.ft & 15, (tpc + 16) // 8))
    if tl is not None and tl.kind == "jump":
        out.append("    jt = ((u32)vu->vi[%d] * 8u) & 0x%04Xu;"
                   % (tl.fs & 15, cfg.pcmask))
    emit_upper(term, out)
    if term.ibit:
        out.append("    { u32 t = 0x%08Xu; memcpy(&vu->i, &t, 4); }" % term.lo_word)

    slot = tpc + 8
    if term_idx + 1 < len(pairs) and pairs[term_idx + 1] == slot:
        _emit_pair(code, slot, out)

    if term.ebit:
        out.append("    pc = 0x%04Xu; goto vu1_done;" % ((slot + 8) & cfg.pcmask))
        return
    if tl is None or tl.kind not in ("branch", "jump"):
        out.append("    pc = 0x%04Xu; goto vu1_done;" % ((slot + 8) & cfg.pcmask))
        return

    if tl.kind == "jump":
        out.append("    switch (jt) {")
        for b in sorted(cfg.blocks):
            out.append("    case 0x%04Xu: goto B_%04X;" % (b, b))
        out.append("    default: *pc_io = jt; *guard_io = guard;")
        out.append("             *fslot_io = fslot; return 2;")
        out.append("    }")
        return

    def go(addr, indent="    "):
        if addr in cfg.blocks:
            return "%sgoto B_%04X;" % (indent, addr)
        return ("%spc = 0x%04Xu; *pc_io = pc; *guard_io = guard; "
                "*fslot_io = fslot; return 2;" % (indent, addr))

    if cond == "1":
        out.append(go(tl.target))
    else:
        if tl.target in cfg.blocks:
            out.append("    if (taken) goto B_%04X;" % tl.target)
        else:
            out.append("    if (taken) {")
            out.append(go(tl.target, "        "))
            out.append("    }")
        out.append(go((slot + 8) & cfg.pcmask))
