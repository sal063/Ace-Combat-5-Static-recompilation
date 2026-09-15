import json, os
import idaapi, idautils, idc, ida_funcs, ida_bytes, ida_nalt, ida_segment
import ida_xref, ida_ua, ida_name

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

OUT = _cfg("ida_db.json")

segs = []
for i in range(ida_segment.get_segm_qty()):
    s = ida_segment.getnseg(i)
    segs.append(dict(name=ida_segment.get_segm_name(s), start=s.start_ea,
                     end=s.end_ea, perm=int(s.perm), type=int(s.type),
                     bitness=int(s.bitness)))

funcs = []
for ea in idautils.Functions():
    f = ida_funcs.get_func(ea)
    if not f:
        continue
    chunks = []
    it = ida_funcs.func_tail_iterator_t(f)
    ok = it.main()
    while ok:
        c = it.chunk()
        chunks.append([c.start_ea, c.end_ea])
        ok = it.next()
    if not chunks:
        chunks = [[f.start_ea, f.end_ea]]
    funcs.append(dict(
        ea=ea,
        name=ida_name.get_ea_name(ea) or ("sub_%08X" % ea),
        chunks=chunks,
        flags=int(f.flags),
        noret=bool(f.flags & idaapi.FUNC_NORET),
        thunk=bool(f.flags & idaapi.FUNC_THUNK),
        lib=bool(f.flags & idaapi.FUNC_LIB),
    ))

switches = []
for fn in funcs:
    for cs, ce in fn['chunks']:
        ea = cs
        while ea < ce:
            si = idaapi.get_switch_info(ea)
            if si:
                try:
                    targets = list(idautils.CodeRefsFrom(ea, 0))
                except Exception:
                    targets = []
                cases = []
                try:
                    import ida_nalt
                    res = idaapi.calc_switch_cases(ea, si)
                    if res:
                        for i in range(res.cases.size()):
                            for t in res.targets[i:i+1]:
                                pass
                        cases = [int(res.targets[i]) for i in range(res.targets.size())]
                except Exception:
                    cases = []
                switches.append(dict(ea=ea, func=fn['ea'],
                                     jumps=int(si.jumps), ncases=int(si.ncases),
                                     elbase=int(si.elbase),
                                     startea=int(si.startea),
                                     targets=cases or targets))
            ea = idc.next_head(ea, ce)
            if ea == idaapi.BADADDR:
                break

names = {}
for ea, nm in idautils.Names():
    names["%d" % ea] = nm

info = idaapi.get_inf_structure() if hasattr(idaapi, 'get_inf_structure') else None
meta = dict(
    imagebase=idaapi.get_imagebase(),
    entry=idc.get_inf_attr(idc.INF_START_IP) if hasattr(idc, 'get_inf_attr') else 0,
    min_ea=idaapi.inf_get_min_ea() if hasattr(idaapi, 'inf_get_min_ea') else 0,
    max_ea=idaapi.inf_get_max_ea() if hasattr(idaapi, 'inf_get_max_ea') else 0,
    input=ida_nalt.get_input_file_path(),
)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w') as fp:
    json.dump(dict(meta=meta, segments=segs, functions=funcs,
                   switches=switches, names=names), fp)
print("funcs=%d switches=%d names=%d segs=%d -> %s"
      % (len(funcs), len(switches), len(names), len(segs), OUT))
