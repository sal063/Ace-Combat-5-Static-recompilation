import idaapi, idautils, idc, ida_ua, json

import os as _os

def _cfg(_name):
    _root = _os.environ.get("PS2RECOMP_ROOT")
    if not _root:
        try:
            _root = _os.path.dirname(_os.path.dirname(
                _os.path.dirname(_os.path.abspath(__file__))))
        except NameError:
            raise SystemExit("Set PS2RECOMP_ROOT to the ps2recomp checkout.")
    _dir = _os.path.join(_root, "config")
    if not _os.path.isdir(_dir):
        raise SystemExit("No config/ under %s; set PS2RECOMP_ROOT." % _root)
    return _os.path.join(_dir, _name)


BIND = 0x331AB8
CALL = 0x331C88

def back_value(ea, reg, limit=40):
    cur = ea
    hi = {}
    for _ in range(limit):
        cur = idc.prev_head(cur)
        if cur == idc.BADADDR:
            return None
        m = idc.print_insn_mnem(cur)
        o0 = idc.print_operand(cur, 0)
        if o0 != reg:
            continue
        if m in ("li", "addiu", "ori", "daddiu"):
            src = idc.print_operand(cur, 1)
            v = idc.get_operand_value(cur, 2) if idc.get_operand_type(cur, 2) else None
            if m == "li":
                return idc.get_operand_value(cur, 1) & 0xFFFFFFFF
            base = idc.print_operand(cur, 1)
            imm = idc.get_operand_value(cur, 2) & 0xFFFF
            if base == "$zero":
                if m == "ori":
                    return imm
                return imm if imm < 0x8000 else imm - 0x10000
            if base in hi:
                return (hi[base] + (imm if (m != "ori" and imm >= 0x8000) and False else imm)) & 0xFFFFFFFF
            return None
        if m == "lui":
            hi[o0] = (idc.get_operand_value(cur, 1) & 0xFFFF) << 16
            continue
        if m in ("move", "or") and idc.print_operand(cur, 2) in ("$zero", ""):
            reg = idc.print_operand(cur, 1)
            continue
        return None
    return None

out = {"bind": [], "call": []}
for ea in idautils.CodeRefsTo(BIND, 0):
    f = idaapi.get_func(ea)
    sid = back_value(ea, "$a1")
    out["bind"].append(dict(site="0x%X" % ea,
                            fn="0x%X" % (f.start_ea if f else 0),
                            sid=("0x%08X" % sid) if sid is not None else None))
for ea in idautils.CodeRefsTo(CALL, 0):
    f = idaapi.get_func(ea)
    fno = back_value(ea, "$a1")
    out["call"].append(dict(site="0x%X" % ea,
                            fn="0x%X" % (f.start_ea if f else 0),
                            fno=("0x%X" % fno) if fno is not None else None))
open(_cfg("rpc_sids.json"), "w").write(json.dumps(out, indent=1))
print("bind sites: %d  call sites: %d" % (len(out["bind"]), len(out["call"])))
known = sorted(set(b["sid"] for b in out["bind"] if b["sid"]))
print("service ids:", known)
print("unresolved binds:", [b["site"] for b in out["bind"] if not b["sid"]])
