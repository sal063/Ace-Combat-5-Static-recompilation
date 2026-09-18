import re
import idautils, ida_funcs

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


OUT = _cfg("game_symbols.txt")

PAT = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)::(~?[A-Za-z_][A-Za-z0-9_]*)")

per_func = {}
for s in idautils.Strings():
    text = str(s)
    names = set("%s::%s" % m for m in PAT.findall(text))
    if not names:
        continue
    for x in idautils.XrefsTo(s.ea):
        f = ida_funcs.get_func(x.frm)
        if f:
            per_func.setdefault(f.start_ea, set()).update(names)

READ = {
    0x00324568: ("CTxtio::_GetS8",     "type 0, returns a signed byte"),
    0x00324688: ("CTxtio::_GetU8",     "type 1, returns an unsigned byte"),
    0x003247B0: ("CTxtio::_GetS16",    "type 2, returns a signed short"),
    0x00324910: ("CTxtio::_GetU16",    "type 3, returns an unsigned short"),
    0x00324A68: ("CTxtio::_GetS32",    "type 4, returns an int"),
    0x00324BC0: ("CTxtio::_GetInt",    "types 5 and 6, returns an int"),
    0x00324D10: ("CTxtio::_GetFloat",  "type 7, returns a float in $f0"),
    0x00324E68: ("CTxtio::_GetString", "type 8, copies 128 bytes to arg 2"),
    0x00325020: ("CTxtio::_GetData",   "type 9, copies 256 bytes to arg 2"),
    0x0031C388: ("main_Loop",          "the frame loop: pad, scene dispatch, draw"),
    0x0031CF00: ("scene_Dispatch",     "calls table[obj+8][obj+9] of a scene machine"),
    0x0031CF88: ("scene_Next",         "commits the pending state at obj+10"),
    0x00101B70: ("pac_ReadTable",      "reads DATA.TBL, or datapack.bin in mode 0"),
    0x00101D98: ("pac_QueueMember",    "appends a member to the load (at most 7)"),
    0x00101DE0: ("pac_BufferSize",     "sums the queued members' unpacked sizes"),
    0x00102060: ("pac_StartLoad",      "sets the buffer and starts the load"),
    0x001020D8: ("pac_Poll",           "the loader state machine, polled per frame"),
    0x001029E0: ("pac_FileInMember",   "a file's address inside a loaded member"),
    0x00102A58: ("pac_MemberBase",     "a loaded member's base address"),
    0x00102A78: ("ulz_Setup",          "ULZ decoder set-up (state, packed, buffer)"),
    0x00102BF8: ("ulz_Step",           "ULZ decode, 2,500 control words a call"),
    0x0031E018: ("bundle_Attach",      "count/base/offsets view of a member"),
    0x0031E030: ("bundle_File",        "base + offset, or 0 for an absent file"),
    0x00320098: ("CDoubleBuff::_Open", "sets +21, returns the write pointer at +8"),
    0x00320390: ("COt::_Init",         "writes a `next` link per bucket into the buffer"),
    0x00320538: ("COt::_Open",         "links the bucket's tail to the buffer write pointer"),
    0x003205D0: ("COt::_Close",        "links the range end back to the bucket, closes at end+16"),
    0x00320648: ("COt::_Relink",       "one `next` from the table end to a fresh buffer range"),
    0x003265E8: ("DrawCtrl::_Flush",   "terminates both packet streams and kicks D1/D2"),
    0x0031FB88: ("prim_Write",         "PRIM/TEX0 then a PACKED run of 36-byte 2D vertices"),
    0x0031FF00: ("prim_Rect",          "a sprite from x, y, w, h and a colour via prim_Write"),
    0x0031FFB0: ("prim_Line",          "a line from two points and a colour via prim_Write"),
    0x0031F2B0: ("tex_Upload",         "BITBLTBUF/TRXPOS/TRXREG/TRXDIR from a texture object"),
    0x00160698: ("mission_Render",     "per-frame mission render: sky, player, message window"),
    0x00113EC8: ("sky_Render",         "sky object: buckets 1, 3, 9, 10 (sun Z sample), 12"),
    0x00114680: ("sky_SunEffects",     "occlusion chain, flare and lens ghosts into bucket 12"),
    0x0011A888: ("sun_ZSample",        "copies scene Z around the sun into a Z24 buffer"),
    0x0011AAF0: ("sun_OcclusionChain", "box sample, depth test, box filter, CLUT alpha writes"),
    0x00118BC8: ("sun_Flare",          "projects the sun, one textured eight-point fan"),
    0x001B0680: ("sun_LensGhosts",     "eight sprites on the sun-to-centre axis"),
    0x0012BEB0: ("player_Render",      "player object: HUD in bucket 11, cockpit and model"),
    0x00133E00: ("hud_Draw",           "flight HUD: ~40 element writers over prim_Write"),
    0x002D6E00: ("msgwin_Draw",        "radio caption window: name and text glyph sprites"),
}

lines = []
seen_names = {}
for ea, names in sorted(per_func.items()):
    if ea in READ or len(names) != 1:
        continue
    name = next(iter(names))
    seen_names.setdefault(name, []).append(ea)
for name, eas in seen_names.items():
    for ea in eas:
        final = name if len(eas) == 1 else "%s@%08X" % (name, ea)
        lines.append((ea, final, "debug string"))
for ea, (name, why) in READ.items():
    lines.append((ea, name, "read from the code: " + why))
lines.sort()

with open(OUT, "w", newline="\n") as fp:
    fp.write("# Names for SLUS_208.51's functions, recovered by "
             "tools/ida/export_game_symbols.py.\n")
    fp.write("# ADDRESS NAME  # where the name comes from\n")
    for ea, name, why in lines:
        fp.write("%08X %s  # %s\n" % (ea, name, why))
print("%d names -> %s (%d functions left unnamed: their messages name more "
      "than one method)" % (len(lines), OUT,
                            sum(1 for n in per_func.values() if len(n) > 1)))
