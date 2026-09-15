import json, struct
import idaapi, idautils, idc, ida_bytes, ida_segment, ida_funcs, ida_xref

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

OUT = _cfg("ida_seeds.json")

tseg = ida_segment.get_segm_by_name(".text")
TLO, THI = tseg.start_ea, tseg.end_ea


def is_code_head(ea):
    if not (TLO <= ea < THI) or (ea & 3):
        return False
    f = ida_bytes.get_flags(ea)
    return ida_bytes.is_code(f) and ida_bytes.is_head(f)


xref_seeds = set()
ea = TLO
while ea < THI:
    if is_code_head(ea):
        for frm in idautils.DataRefsTo(ea):
            xref_seeds.add(ea)
            break
    ea = idc.next_head(ea, THI) if idc.next_head(ea, THI) != idaapi.BADADDR else ea + 4

ptr_seeds = set()
scanned = 0
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    nm = ida_segment.get_segm_name(s)
    if nm in (".text", ".vutext"):
        continue
    size = s.end_ea - s.start_ea
    if size <= 0 or size > 0x400000:
        continue
    blob = ida_bytes.get_bytes(s.start_ea, size)
    if not blob:
        continue
    scanned += size
    n = len(blob) & ~3
    for off in range(0, n, 4):
        v = struct.unpack_from("<I", blob, off)[0]
        if TLO <= v < THI and (v & 3) == 0:
            if is_code_head(v):
                ptr_seeds.add(v)

flow_seeds = set()
for fn_ea in idautils.Functions():
    f = ida_funcs.get_func(fn_ea)
    if not f:
        continue
    it = ida_funcs.func_tail_iterator_t(f)
    ok = it.main()
    while ok:
        c = it.chunk()
        ea = c.start_ea
        while ea < c.end_ea:
            for t in idautils.CodeRefsFrom(ea, 0):
                if is_code_head(t):
                    flow_seeds.add(t)
            nx = idc.next_head(ea, c.end_ea)
            ea = nx if nx != idaapi.BADADDR else c.end_ea
        ok = it.next()

data = dict(xref=sorted(xref_seeds), ptr=sorted(ptr_seeds), flow=sorted(flow_seeds))
with open(OUT, "w") as fp:
    json.dump(data, fp)
print("xref=%d ptr=%d flow=%d (scanned %d data bytes) -> %s"
      % (len(xref_seeds), len(ptr_seeds), len(flow_seeds), scanned, OUT))
