import json
import os as _os
import re
import struct

import ida_bytes
import ida_funcs
import ida_segment
import idautils
import idc


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


OUT = _cfg("render_emitters.json")

OT_OPEN = 0x00320538
DB_OPEN = 0x00320098
HELPERS_2D = [0x0032B3F0, 0x0032B5B0, 0x0032B650, 0x0032B6F0, 0x0032B850,
              0x0032B928]
_text = ida_segment.get_segm_by_name(".text")
TEXT_LO, TEXT_HI = _text.start_ea, _text.end_ea


def rtti_classes():
    pat = re.compile(r"^(\d+)([A-Za-z_][A-Za-z0-9_]*)$")
    tf = {}
    for s in idautils.Strings():
        m = pat.match(str(s))
        if not m or int(m.group(1)) != len(m.group(2)):
            continue
        for x in idautils.XrefsTo(s.ea):
            f = ida_funcs.get_func(x.frm)
            if f and f.size() < 0x80:
                tf[f.start_ea] = m.group(2)
    return tf


def vtable_methods(tf):
    blobs = []
    for seg in idautils.Segments():
        if idc.get_segm_name(seg) in (".data", ".rodata", ".sdata"):
            end = idc.get_segm_end(seg)
            blobs.append((seg, ida_bytes.get_bytes(seg, end - seg)))
    methods = {}
    vtables = {}
    for fea, cls in tf.items():
        needle = struct.pack("<I", fea)
        for base, data in blobs:
            i = data.find(needle)
            while i >= 0:
                if i % 4 == 0 and i >= 4:
                    slot0 = base + i - 4
                    vtables.setdefault(cls, []).append(slot0)
                    k = 1
                    while True:
                        pfn = ida_bytes.get_dword(slot0 + 8 * k + 4)
                        f = ida_funcs.get_func(pfn)
                        if not (TEXT_LO <= pfn < TEXT_HI) or not f \
                                or f.start_ea != pfn:
                            break
                        methods.setdefault(pfn, set()).add(
                            "%s::vf%d" % (cls, k - 1))
                        k += 1
                i = data.find(needle, i + 1)
    return methods, vtables


def string_names():
    pat = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)::(~?[A-Za-z_][A-Za-z0-9_]*)")
    per = {}
    for s in idautils.Strings():
        found = set("%s::%s" % m for m in pat.findall(str(s)))
        if not found:
            continue
        for x in idautils.XrefsTo(s.ea):
            f = ida_funcs.get_func(x.frm)
            if f:
                per.setdefault(f.start_ea, set()).update(found)
    return {ea: next(iter(n)) for ea, n in per.items() if len(n) == 1}


def name_of(ea, strnames, methods):
    if ea in strnames:
        return strnames[ea]
    if ea in methods:
        return " / ".join(sorted(methods[ea])[:3])
    nm = idc.get_func_name(ea)
    return nm if nm and not nm.startswith("sub_") else None


def callers(ea):
    out = set()
    for x in idautils.XrefsTo(ea):
        f = ida_funcs.get_func(x.frm)
        if f and idc.print_insn_mnem(x.frm).startswith("j"):
            out.add(f.start_ea)
    return out


def ancestors(ea, strnames, methods, depth=3):
    seen, frontier, found = {ea}, [ea], []
    for level in range(1, depth + 1):
        nxt = []
        for f in frontier:
            for c in sorted(callers(f)):
                if c in seen:
                    continue
                seen.add(c)
                nxt.append(c)
                nm = name_of(c, strnames, methods)
                if nm:
                    found.append({"level": level, "func": "%08X" % c,
                                  "name": nm})
        frontier = nxt
        if len(seen) > 400:
            break
    return found


def const_arg(site, reg):
    cands = [idc.next_head(site)]
    p = site
    for _ in range(12):
        p = idc.prev_head(p)
        cands.append(p)
    for c in cands:
        if idc.print_operand(c, 0) != reg:
            continue
        m = idc.print_insn_mnem(c)
        if m == "li":
            return idc.get_operand_value(c, 1) & 0xFFFFFFFF
        if m == "move" and idc.print_operand(c, 1) == "$zero":
            return 0
        return None
    return None


def main():
    tf = rtti_classes()
    methods, vtables = vtable_methods(tf)
    strnames = string_names()
    emitters = {}
    for x in idautils.XrefsTo(OT_OPEN):
        if not idc.print_insn_mnem(x.frm).startswith("jal"):
            continue
        f = ida_funcs.get_func(x.frm)
        if not f:
            continue
        e = emitters.setdefault(f.start_ea, {"sites": []})
        e["sites"].append({"site": "%08X" % (x.frm + 8),
                           "bucket": const_arg(x.frm, "$a1")})
    for h in HELPERS_2D:
        emitters.setdefault(h, {"sites": []})["helper_2d"] = True
    for x in idautils.XrefsTo(DB_OPEN):
        f = ida_funcs.get_func(x.frm)
        if f and f.start_ea not in emitters:
            emitters.setdefault(f.start_ea, {"sites": []})["direct"] = True
    out = {}
    for ea, e in sorted(emitters.items()):
        f = ida_funcs.get_func(ea)
        out["%08X" % ea] = {
            "name": name_of(ea, strnames, methods),
            "size": f.size() if f else 0,
            "sites": e["sites"],
            "helper_2d": e.get("helper_2d", False),
            "direct": e.get("direct", False),
            "ancestors": ancestors(ea, strnames, methods),
        }
    doc = {
        "binary": idc.get_root_filename(),
        "classes_with_vtables": len(vtables),
        "virtual_methods": {"%08X" % k: sorted(v) for k, v in
                            sorted(methods.items())},
        "emitters": out,
    }
    with open(OUT, "w", newline="\n") as fp:
        json.dump(doc, fp, indent=1, sort_keys=True)
    print("%d emitter functions, %d classes with vtables, %d virtual methods "
          "-> %s" % (len(out), len(vtables), len(methods), OUT))


main()
