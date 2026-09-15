import os
import struct
from collections import defaultdict

R_MIPS_NONE = 0
R_MIPS_16 = 1
R_MIPS_32 = 2
R_MIPS_REL32 = 3
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
R_MIPS_GPREL16 = 7
R_MIPS_LITERAL = 8
R_MIPS_GOT16 = 9
R_MIPS_PC16 = 10
R_MIPS_CALL16 = 11
R_MIPS_GPREL32 = 12

RELOC_MASK = {
    R_MIPS_16: 0x0000FFFF,
    R_MIPS_32: 0xFFFFFFFF,
    R_MIPS_REL32: 0xFFFFFFFF,
    R_MIPS_26: 0x03FFFFFF,
    R_MIPS_HI16: 0x0000FFFF,
    R_MIPS_LO16: 0x0000FFFF,
    R_MIPS_GPREL16: 0x0000FFFF,
    R_MIPS_LITERAL: 0x0000FFFF,
    R_MIPS_GOT16: 0x0000FFFF,
    R_MIPS_PC16: 0x0000FFFF,
    R_MIPS_CALL16: 0x0000FFFF,
    R_MIPS_GPREL32: 0xFFFFFFFF,
}

SHT_SYMTAB = 2
SHT_REL = 9
STT_FUNC = 2


def ar_members(data):
    if data[:8] != b"!<arch>\n":
        return []
    off, longnames, out = 8, b"", []
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        raw = hdr[0:16].decode("latin1")
        try:
            size = int(hdr[48:58].decode("latin1").strip() or 0)
        except ValueError:
            break
        off += 60
        body = data[off:off + size]
        off += size + (size & 1)
        name = raw.rstrip()
        if name.startswith("//"):
            longnames = body
            continue
        if name.startswith("/") and name[1:].strip().isdigit():
            i = int(name[1:].strip())
            end = longnames.find(b"/", i)
            name = longnames[i:end].decode("latin1")
        name = name.rstrip("/")
        if name in ("/", ""):
            continue
        out.append((name, body))
    return out


class Obj:
    def __init__(self, blob):
        self.d = blob
        (e_shoff,) = struct.unpack_from("<I", blob, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
        self.secs = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            (nm, typ, flags, addr, offset, size, link, info, align,
             entsize) = struct.unpack_from("<10I", blob, o)
            self.secs.append(dict(name_off=nm, type=typ, flags=flags, addr=addr,
                                  offset=offset, size=size, link=link,
                                  info=info, entsize=entsize))
        sh = self.secs[e_shstrndx]
        strs = blob[sh["offset"]:sh["offset"] + sh["size"]]
        for s in self.secs:
            e = strs.find(b"\0", s["name_off"])
            s["name"] = strs[s["name_off"]:e].decode("latin1")

    def symbols(self):
        out = []
        for s in self.secs:
            if s["type"] != SHT_SYMTAB:
                continue
            st = self.secs[s["link"]]
            strs = self.d[st["offset"]:st["offset"] + st["size"]]
            for i in range(s["size"] // 16):
                o = s["offset"] + i * 16
                nameoff, value, size, info, other, shndx = struct.unpack_from(
                    "<IIIBBH", self.d, o)
                e = strs.find(b"\0", nameoff)
                out.append(dict(name=strs[nameoff:e].decode("latin1"),
                                value=value, size=size, type=info & 0xF,
                                bind=info >> 4, shndx=shndx))
        return out

    def relocs_for(self, sec_index):
        out = []
        for s in self.secs:
            if s["type"] != SHT_REL or s["info"] != sec_index:
                continue
            n = s["size"] // 8
            for i in range(n):
                off, info = struct.unpack_from("<II", self.d,
                                               s["offset"] + i * 8)
                out.append((off, info & 0xFF))
        return out


def build_signatures(lib_dir, min_words=6, log=print):
    sigs = []
    for fn in sorted(os.listdir(lib_dir)):
        if not fn.endswith(".a"):
            continue
        path = os.path.join(lib_dir, fn)
        try:
            data = open(path, "rb").read()
        except OSError:
            continue
        for member, blob in ar_members(data):
            if blob[:4] != b"\x7fELF":
                continue
            try:
                obj = Obj(blob)
            except Exception:
                continue
            for si, sec in enumerate(obj.secs):
                if not sec["name"].startswith(".text"):
                    continue
                rel = obj.relocs_for(si)
                relmap = {}
                for off, typ in rel:
                    relmap[off] = relmap.get(off, 0) | RELOC_MASK.get(typ, 0)
                body = blob[sec["offset"]:sec["offset"] + sec["size"]]
                for sym in obj.symbols():
                    if sym["type"] != STT_FUNC or sym["shndx"] != si:
                        continue
                    size = sym["size"]
                    if size < min_words * 4:
                        continue
                    start = sym["value"]
                    if start + size > len(body):
                        continue
                    words, masks = [], []
                    for k in range(0, size - 3, 4):
                        w = struct.unpack_from("<I", body, start + k)[0]
                        m = relmap.get(start + k, 0)
                        words.append(w & ~m & 0xFFFFFFFF)
                        masks.append(~m & 0xFFFFFFFF)
                    if len(words) < min_words:
                        continue
                    sigs.append((sym["name"], fn, words, masks))
    log("signatures: %d functions from %s" % (len(sigs), lib_dir))
    return sigs


def match(sigs, prog, log=print):
    text = []
    for lo, hi in prog.text:
        text.append((lo, hi))

    by_first = defaultdict(list)
    for name, lib, words, masks in sigs:
        idx = 0
        while idx < len(words) and masks[idx] != 0xFFFFFFFF:
            idx += 1
        if idx >= len(words) or idx > 3:
            continue
        by_first[(idx, words[idx])].append((name, lib, words, masks))

    found = {}
    ambiguous = set()
    entries = sorted(prog.entries)
    for ea in entries:
        for idx in range(4):
            probe = prog.words.get(ea + idx * 4)
            if probe is None:
                continue
            for name, lib, words, masks in by_first.get((idx, probe), ()):
                ok = True
                for k in range(len(words)):
                    gw = prog.words.get(ea + k * 4)
                    if gw is None or (gw & masks[k]) != words[k]:
                        ok = False
                        break
                if ok:
                    if ea in found and found[ea][0] != name:
                        ambiguous.add(ea)
                    else:
                        found[ea] = (name, lib, len(words))
    log("matched %d SDK functions (%d ambiguous)" % (len(found), len(ambiguous)))
    return {a: v for a, v in found.items() if a not in ambiguous}


def anchor_object_symbols(lib_dir, prog, found, log=print):
    extra = {}
    for fn in sorted(os.listdir(lib_dir)):
        if not fn.endswith(".a"):
            continue
        try:
            data = open(os.path.join(lib_dir, fn), "rb").read()
        except OSError:
            continue
        for member, blob in ar_members(data):
            if blob[:4] != b"\x7fELF":
                continue
            try:
                obj = Obj(blob)
            except Exception:
                continue
            for si, sec in enumerate(obj.secs):
                if not sec["name"].startswith(".text"):
                    continue
                syms = [s for s in obj.symbols()
                        if s["type"] == STT_FUNC and s["shndx"] == si]
                if not syms:
                    continue
                byname = {s["name"]: s for s in syms}
                offsets = defaultdict(int)
                for addr, (name, lib, _n) in found.items():
                    if lib != fn or name not in byname:
                        continue
                    offsets[addr - byname[name]["value"]] += 1
                if not offsets:
                    continue
                best = max(offsets.items(), key=lambda kv: kv[1])
                if best[1] < 2:
                    continue
                base = best[0]
                for s in syms:
                    a = base + s["value"]
                    if a in found or a in extra:
                        continue
                    if a not in prog.entries:
                        continue
                    extra[a] = (s["name"], fn + " (anchored)", 0)
    log("anchored %d further symbols from object layout" % len(extra))
    found.update(extra)
    return found


def align_object_symbols(lib_dir, prog, found, log=print):
    entries = sorted(prog.functions)
    extra, rejected = {}, []
    byname = {}
    for a, (n, l, _c) in found.items():
        byname.setdefault(n, []).append(a)

    for fn in sorted(os.listdir(lib_dir)):
        if not fn.endswith(".a"):
            continue
        try:
            data = open(os.path.join(lib_dir, fn), "rb").read()
        except OSError:
            continue
        for member, blob in ar_members(data):
            if blob[:4] != b"\x7fELF":
                continue
            try:
                obj = Obj(blob)
            except Exception:
                continue
            for si, sec in enumerate(obj.secs):
                if not sec["name"].startswith(".text"):
                    continue
                syms = sorted((s for s in obj.symbols()
                               if s["type"] == STT_FUNC and s["shndx"] == si
                               and s["size"] > 0),
                              key=lambda s: s["value"])
                if not syms:
                    continue
                cp = {}
                for s in syms:
                    for a in byname.get(s["name"], ()):
                        if found[a][1] == fn:
                            cp[s["name"]] = a
                if not cp or syms[0]["name"] not in cp or syms[-1]["name"] not in cp:
                    continue
                first = min(cp.items(), key=lambda kv: syms.index(
                    next(s for s in syms if s["name"] == kv[0])))
                fidx = next(i for i, s in enumerate(syms) if s["name"] == first[0])
                try:
                    epos = entries.index(first[1])
                except ValueError:
                    continue
                if epos - fidx < 0:
                    continue
                zipped = entries[epos - fidx: epos - fidx + len(syms)]
                if len(zipped) != len(syms):
                    continue
                ok = True
                for s, a in zip(syms, zipped):
                    want = cp.get(s["name"])
                    if want is not None and want != a:
                        ok = False
                        break
                    if a in found and found[a][0] != s["name"]:
                        ok = False
                        break
                    if a in extra and extra[a][0] != s["name"]:
                        ok = False
                        break
                    ai = entries.index(a)
                    if ai + 1 >= len(entries) or abs(entries[ai + 1] - a - s["size"]) > 32:
                        ok = False
                        break
                if not ok:
                    rejected.append("%s(%s)" % (fn, member))
                    continue
                for s, a in zip(syms, zipped):
                    if a in found or a in extra:
                        continue
                    extra[a] = (s["name"], fn + " (aligned)", 0)
    log("aligned %d further symbols by object order (%d objects rejected)"
        % (len(extra), len(rejected)))
    if rejected:
        log("   rejected: %s" % ", ".join(sorted(set(rejected))[:12]))
    found.update(extra)
    return found
