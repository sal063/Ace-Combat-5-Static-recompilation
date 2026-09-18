import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from vurecomp import vudec

PROGRAMS = [
    (0x39DB10, 0x39FEA0), (0x39DB10, 0x39FF90), (0x3A0080, 0x3A1100),
    (0x3A1110, 0x3A1D30), (0x3A1D40, 0x3A40B0), (0x3A40C0, 0x3A6600),
    (0x3A6610, 0x3A91B0), (0x3A9350, 0x3AA3B0), (0x3AA410, 0x3ACE40),
    (0x3ACFE0, 0x3AE100), (0x3AE110, 0x3B0D50), (0x3B4080, 0x3B7830),
    (0x3BABD0, 0x3BC220), (0x3BC600, 0x3BD980), (0x3BE3E0, 0x3BEF00),
    (0x3BEF10, 0x3C0650), (0x3C06F0, 0x3C2230), (0x3C2240, 0x3C3D70),
    (0x3C3D80, 0x3C5210), (0x3C5220, 0x3C6ED0),
]


def load_elf(path):
    data = open(path, "rb").read()
    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    phnum = struct.unpack_from("<H", data, 0x2C)[0]
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from(
            "<IIIIIIII", data, phoff + i * 32)
        if p_type == 1:
            segs.append((p_vaddr, data[p_off:p_off + p_filesz]))

    def read(addr, n):
        for va, blob in segs:
            if va <= addr and addr + n <= va + len(blob):
                return blob[addr - va:addr - va + n]
        raise ValueError("address %08X not in a loaded segment" % addr)
    return read


def build_image(read, start, end):
    image = bytearray(16384)
    written = set()
    pos = start + 8
    while pos + 4 <= end:
        code = struct.unpack("<I", read(pos, 4))[0]
        pos += 4
        cmd = (code >> 24) & 0x7F
        if cmd == 0x4A:
            n = ((code >> 16) & 0xFF) or 256
            addr = (code & 0xFFFF) * 8
            image[addr:addr + n * 8] = read(pos, n * 8)
            written.update(range(addr, addr + n * 8, 8))
            pos += n * 8
        elif cmd in (0x00, 0x10, 0x11, 0x13):
            continue
        else:
            break
    return bytes(image), written


FIELD = "xyzw"


def _m(ins):
    m = ins.mask()
    return "" if m == "xyzw" else "." + m


def _up(ins):
    n, ft, fs, fd = ins.name, ins.ft, ins.fs, ins.fd
    m = _m(ins)
    ops = {"ADD": "+", "SUB": "-", "MUL": "*"}
    for base, op in ops.items():
        if n == base:
            return "vf%d%s = vf%d %s vf%d" % (fd, m, fs, op, ft)
        if n in (base + c for c in FIELD):
            return "vf%d%s = vf%d %s vf%d.%s" % (fd, m, fs, op, ft, n[-1])
        if n == base + "i":
            return "vf%d%s = vf%d %s I" % (fd, m, fs, op)
        if n == base + "q":
            return "vf%d%s = vf%d %s Q" % (fd, m, fs, op)
        if n == base + "A":
            return "ACC%s = vf%d %s vf%d" % (m, fs, op, ft)
        if n in (base + "A" + c for c in FIELD):
            return "ACC%s = vf%d %s vf%d.%s" % (m, fs, op, ft, n[-1])
        if n == base + "Ai":
            return "ACC%s = vf%d %s I" % (m, fs, op)
        if n == base + "Aq":
            return "ACC%s = vf%d %s Q" % (m, fs, op)
    for base, op in (("MADD", "+"), ("MSUB", "-")):
        if n == base:
            return "vf%d%s = ACC %s vf%d * vf%d" % (fd, m, op, fs, ft)
        if n in (base + c for c in FIELD):
            return "vf%d%s = ACC %s vf%d * vf%d.%s" % (fd, m, op, fs, ft, n[-1])
        if n == base + "i":
            return "vf%d%s = ACC %s vf%d * I" % (fd, m, op, fs)
        if n == base + "q":
            return "vf%d%s = ACC %s vf%d * Q" % (fd, m, op, fs)
        if n == base + "A":
            return "ACC%s = ACC %s vf%d * vf%d" % (m, op, fs, ft)
        if n in (base + "A" + c for c in FIELD):
            return "ACC%s = ACC %s vf%d * vf%d.%s" % (m, op, fs, ft, n[-1])
        if n == base + "Ai":
            return "ACC%s = ACC %s vf%d * I" % (m, op, fs)
        if n == base + "Aq":
            return "ACC%s = ACC %s vf%d * Q" % (m, op, fs)
    if n.startswith("MAX") or n.startswith("MINI"):
        f = "max" if n.startswith("MAX") else "min"
        rest = n[3:] if f == "max" else n[4:]
        other = "vf%d" % ft if rest == "" else ("I" if rest == "i" else "vf%d.%s" % (ft, rest))
        return "vf%d%s = %s(vf%d, %s)" % (fd, m, f, fs, other)
    if n.startswith("ITOF") or n.startswith("FTOI"):
        return "vf%d%s = %s(vf%d)" % (ft, m, n.lower(), fs)
    if n == "ABS":
        return "vf%d%s = abs(vf%d)" % (ft, m, fs)
    if n == "CLIP":
        return "CLIP(vf%d.xyz vs vf%d.w)" % (fs, ft)
    if n == "OPMULA":
        return "ACC.xyz = vf%d.yzx * vf%d.zxy" % (fs, ft)
    if n == "OPMSUB":
        return "vf%d.xyz = ACC - vf%d.zxy * vf%d.yzx" % (fd, fs, ft)
    if n == "NOP":
        return ""
    return n


def _lo(ins):
    n = ins.name
    it, is_, id_ = ins.ft, ins.fs, ins.fd
    m = _m(ins)
    fsf = FIELD[ins.fsf]
    ftf = FIELD[ins.ftf]
    imm = ins.imm
    if n == "MOVE":
        return "" if ins.dest == 0 else "vf%d%s = vf%d" % (it, m, is_)
    if n == "MR32":
        return "vf%d%s = vf%d.yzwx" % (it, m, is_)
    if n == "LQ":
        return "vf%d%s = [vi%d + %03X]" % (it, m, is_, imm & 0xFFFF)
    if n == "SQ":
        return "[vi%d + %03X]%s = vf%d" % (it, imm & 0xFFFF, m, is_)
    if n == "LQI":
        return "vf%d%s = [vi%d++]" % (it, m, is_)
    if n == "SQI":
        return "[vi%d++]%s = vf%d" % (it, m, is_)
    if n == "LQD":
        return "vf%d%s = [--vi%d]" % (it, m, is_)
    if n == "SQD":
        return "[--vi%d]%s = vf%d" % (it, m, is_)
    if n == "ILW":
        return "vi%d = [vi%d + %03X]%s" % (it, is_, imm & 0xFFFF, m)
    if n == "ISW":
        return "[vi%d + %03X]%s = vi%d" % (is_, imm & 0xFFFF, m, it)
    if n == "ILWR":
        return "vi%d = [vi%d]%s" % (it, is_, m)
    if n == "ISWR":
        return "[vi%d]%s = vi%d" % (is_, m, it)
    if n in ("IADD", "ISUB", "IAND", "IOR"):
        op = {"IADD": "+", "ISUB": "-", "IAND": "&", "IOR": "|"}[n]
        return "vi%d = vi%d %s vi%d" % (id_, is_, op, it)
    if n == "IADDI":
        return "vi%d = vi%d + %d" % (it, is_, imm)
    if n == "IADDIU":
        return "vi%d = vi%d + 0x%X" % (it, is_, imm)
    if n == "ISUBIU":
        return "vi%d = vi%d - 0x%X" % (it, is_, imm)
    if n == "DIV":
        return "Q = vf%d.%s / vf%d.%s" % (is_, fsf, it, ftf)
    if n == "SQRT":
        return "Q = sqrt(vf%d.%s)" % (it, ftf)
    if n == "RSQRT":
        return "Q = vf%d.%s / sqrt(vf%d.%s)" % (is_, fsf, it, ftf)
    if n == "MTIR":
        return "vi%d = (s16)vf%d.%s" % (it, is_, fsf)
    if n == "MFIR":
        return "vf%d%s = itof(vi%d)" % (it, m, is_)
    if n in ("FCEQ", "FCSET", "FCAND", "FCOR"):
        return "%s 0x%06X" % (n, imm)
    if n in ("FSEQ", "FSSET", "FSAND", "FSOR"):
        return "%s 0x%03X" % (n, imm)
    if n in ("FMEQ", "FMAND", "FMOR"):
        return "vi%d = %s(MAC, vi%d)" % (it, n, is_)
    if n == "FCGET":
        return "vi%d = clipflags & 0xFFF" % it
    if n == "B":
        return "goto %04X" % ins.target
    if n == "BAL":
        return "vi%d = ret; goto %04X" % (it, ins.target)
    if n == "JR":
        return "goto vi%d" % is_
    if n == "JALR":
        return "vi%d = ret; goto vi%d" % (it, is_)
    cond = {"IBEQ": "vi%d == vi%d" % (it, is_), "IBNE": "vi%d != vi%d" % (it, is_),
            "IBLTZ": "vi%d < 0" % is_, "IBGTZ": "vi%d > 0" % is_,
            "IBLEZ": "vi%d <= 0" % is_, "IBGEZ": "vi%d >= 0" % is_}
    if n in cond:
        return "if (%s) goto %04X" % (cond[n], ins.target)
    if n == "XTOP":
        return "vi%d = TOP" % it
    if n == "XITOP":
        return "vi%d = ITOP" % it
    if n == "XGKICK":
        return "XGKICK vi%d" % is_
    if n == "WAITQ":
        return "WAITQ"
    if n == "MFP":
        return "vf%d%s = P" % (it, m)
    efu = {"ELENG": "P = len(vf%d.xyz)", "ERLENG": "P = 1 / len(vf%d.xyz)",
           "ESADD": "P = vf%d.x^2 + vf%d.y^2 + vf%d.z^2",
           "ERSADD": "P = 1 / (vf%d.x^2 + vf%d.y^2 + vf%d.z^2)",
           "ESUM": "P = vf%d.x + vf%d.y + vf%d.z + vf%d.w"}
    if n in efu:
        return efu[n] % ((is_,) * efu[n].count("%d"))
    if n in ("ERCPR", "ESQRT", "ERSQRT", "ESIN", "EATAN", "EEXP"):
        return "P = %s(vf%d.%s)" % (n.lower(), is_, fsf)
    return n


def fmt(ins):
    if ins is None:
        return ""
    return _up(ins) if ins.pipe == "upper" else _lo(ins)


def listing(image, written):
    pairs = vudec.decode_image(image)
    out = []
    for pc in sorted(written):
        p = pairs[pc]
        flags = ("I" if p.ibit else "-") + ("E" if p.ebit else "-")
        if p.ibit:
            f = struct.unpack("<f", struct.pack("<I", p.lo_word))[0]
            lo = "imm %08X (%g)" % (p.lo_word, f)
        else:
            lo = fmt(p.lower)
        up = fmt(p.upper)
        if not up and not lo and not p.ebit:
            continue
        out.append("%04X %s  %-40s %s" % (pc, flags, up, lo))
    return "\n".join(out)


def main():
    if len(sys.argv) < 3:
        print("usage: python tools/vu_listing.py <SLUS_208.51> <outdir> [start:end ...]")
        return 1
    read = load_elf(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    progs = PROGRAMS
    if len(sys.argv) > 3:
        progs = [tuple(int(x, 16) for x in a.split(":")) for a in sys.argv[3:]]
    for start, end in progs:
        image, written = build_image(read, start, end)
        path = os.path.join(outdir, "vu_%06X_%06X.txt" % (start, end))
        with open(path, "w") as f:
            f.write("; program packet %06X..%06X, %d pairs\n" % (start, end, len(written)))
            f.write(listing(image, written) + "\n")
        print("%06X..%06X  %4d pairs  -> %s" % (start, end, len(written), path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
