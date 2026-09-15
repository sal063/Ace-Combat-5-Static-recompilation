BC = "xyzw"


class Insn:
    __slots__ = ("word", "pipe", "op", "name", "dest", "ft", "fs", "fd",
                 "bc", "fsf", "ftf", "imm", "target", "kind")

    def __init__(self, word, pipe):
        self.word = word
        self.pipe = pipe
        self.op = None
        self.name = "?"
        self.dest = (word >> 21) & 0xF
        self.ft = (word >> 16) & 0x1F
        self.fs = (word >> 11) & 0x1F
        self.fd = (word >> 6) & 0x1F
        self.bc = word & 3
        self.fsf = (word >> 21) & 3
        self.ftf = (word >> 23) & 3
        self.imm = 0
        self.target = None
        self.kind = "alu"

    def mask(self):
        return "".join(c for i, c in enumerate(BC) if (self.dest >> (3 - i)) & 1)

    def __repr__(self):
        return "%-10s %s" % (self.name, self.mask())


_UP_MAIN = {}
for _i, _n in enumerate(("ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL")):
    for _b in range(4):
        _UP_MAIN[_i * 4 + _b] = _n + BC[_b]
_UP_MAIN.update({
    0x1C: "MULq",  0x1D: "MAXi",  0x1E: "MULi",  0x1F: "MINIi",
    0x20: "ADDq",  0x21: "MADDq", 0x22: "ADDi",  0x23: "MADDi",
    0x24: "SUBq",  0x25: "MSUBq", 0x26: "SUBi",  0x27: "MSUBi",
    0x28: "ADD",   0x29: "MADD",  0x2A: "MUL",   0x2B: "MAX",
    0x2C: "SUB",   0x2D: "MSUB",  0x2E: "OPMSUB", 0x2F: "MINI",
})

_UP_ESC = {}
for _i, _n in enumerate(("ADDA", "SUBA", "MADDA", "MSUBA")):
    for _b in range(4):
        _UP_ESC[_i * 4 + _b] = _n + BC[_b]
for _b in range(4):
    _UP_ESC[0x10 + _b] = "ITOF%d" % (0, 4, 12, 15)[_b]
    _UP_ESC[0x14 + _b] = "FTOI%d" % (0, 4, 12, 15)[_b]
    _UP_ESC[0x18 + _b] = "MULA" + BC[_b]
_UP_ESC.update({
    0x1C: "MULAq", 0x1D: "ABS",   0x1E: "MULAi", 0x1F: "CLIP",
    0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi", 0x23: "MADDAi",
    0x24: "SUBAq", 0x25: "MSUBAq", 0x26: "SUBAi", 0x27: "MSUBAi",
    0x28: "ADDA",  0x29: "MADDA", 0x2A: "MULA",
    0x2C: "SUBA",  0x2D: "MSUBA", 0x2E: "OPMULA", 0x2F: "NOP",
})

UPPER_NO_FLAGS = {"MAXx", "MAXy", "MAXz", "MAXw", "MAX", "MAXi",
                  "MINIx", "MINIy", "MINIz", "MINIw", "MINI", "MINIi"}

UPPER_ACC = {n for n in _UP_ESC.values()
             if n.startswith(("ADDA", "SUBA", "MADDA", "MSUBA", "MULA"))}


def decode_upper(word):
    i = Insn(word, "upper")
    funct = word & 0x3F
    if (funct & 0x3C) == 0x3C:
        sub = (word & 3) | ((word >> 4) & 0x7C)
        i.op = ("esc", sub)
        i.name = _UP_ESC.get(sub)
    else:
        i.op = ("main", funct)
        i.name = _UP_MAIN.get(funct)
    if i.name is None:
        i.name = "UNKNOWN"
        i.kind = "bad"
    elif i.name == "NOP":
        i.kind = "nop"
    return i


_LO_MAIN = {
    0x00: "LQ",   0x01: "SQ",   0x04: "ILW",  0x05: "ISW",
    0x08: "IADDIU", 0x09: "ISUBIU",
    0x10: "FCEQ", 0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR",
    0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
    0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET",
    0x20: "B",    0x21: "BAL",  0x24: "JR",   0x25: "JALR",
    0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ",
    0x2E: "IBLEZ", 0x2F: "IBGEZ",
}

_LO_LOW6 = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI",
            0x34: "IAND", 0x35: "IOR"}

_LO_SUB = {
    0x33C: "MOVE",  0x33D: "MR32",
    0x37C: "LQI",   0x37D: "SQI",   0x37E: "LQD",   0x37F: "SQD",
    0x3BC: "DIV",   0x3BD: "SQRT",  0x3BE: "RSQRT", 0x3BF: "WAITQ",
    0x3FC: "MTIR",  0x3FD: "MFIR",  0x3FE: "ILWR",  0x3FF: "ISWR",
    0x43C: "RNEXT", 0x43D: "RGET",  0x43E: "RINIT", 0x43F: "RXOR",
    0x67C: "MFP",   0x6BC: "XTOP",  0x6BD: "XITOP", 0x6FC: "XGKICK",
    0x73C: "ESADD", 0x73D: "ERSADD", 0x73E: "ELENG", 0x73F: "ERLENG",
    0x77C: "EATANxy", 0x77D: "EATANxz", 0x77E: "ESUM",
    0x7BC: "ESQRT", 0x7BD: "ERSQRT", 0x7BE: "ERCPR", 0x7BF: "WAITP",
    0x7FC: "ESIN",  0x7FD: "EATAN", 0x7FE: "EEXP",
}

BRANCH = {"B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"}
JUMP = {"JR", "JALR"}
EFU_LATENCY = {
    "ESADD": 11, "ERSADD": 18, "ELENG": 18, "ERLENG": 24, "ESUM": 12,
    "ESQRT": 12, "ERSQRT": 18, "ERCPR": 12, "ESIN": 29, "EATAN": 54,
    "EATANxy": 54, "EATANxz": 54, "EEXP": 44,
}
FDIV_LATENCY = {"DIV": 7, "SQRT": 7, "RSQRT": 13}

def is_lower_nop(word):
    return (((word >> 25) & 0x7F) == 0x40
            and (word & 0x7FF) == 0x33C
            and ((word >> 21) & 0xF) == 0)


def decode_lower(word, pc, pcmask=0x3FF8):
    i = Insn(word, "lower")
    op = (word >> 25) & 0x7F

    imm11 = word & 0x7FF
    if imm11 & 0x400:
        imm11 -= 0x800
    i.imm = imm11

    if op != 0x40:
        i.op = ("main", op)
        i.name = _LO_MAIN.get(op)
        if i.name is None:
            i.name = "UNKNOWN"
            i.kind = "bad"
            return i
        if i.name in BRANCH:
            i.kind = "branch"
            i.target = (pc + imm11 * 8) & pcmask
        elif i.name in JUMP:
            i.kind = "jump"
        elif i.name in ("LQ", "SQ", "ILW", "ISW"):
            i.kind = "mem"
        if i.name in ("IADDIU", "ISUBIU"):
            i.imm = ((word >> 10) & 0x7800) | (word & 0x7FF)
        elif i.name in ("FCEQ", "FCSET", "FCAND", "FCOR"):
            i.imm = word & 0xFFFFFF
        elif i.name in ("FSEQ", "FSSET", "FSAND", "FSOR"):
            i.imm = word & 0xFFF
        return i

    low6 = word & 0x3F
    if low6 in _LO_LOW6:
        i.op = ("low6", low6)
        i.name = _LO_LOW6[low6]
        if i.name == "IADDI":
            imm5 = (word >> 6) & 0x1F
            i.imm = imm5 - 0x20 if imm5 & 0x10 else imm5
        return i

    sub = word & 0x7FF
    i.op = ("sub", sub)
    i.name = _LO_SUB.get(sub)
    if i.name is None:
        i.name = "UNKNOWN"
        i.kind = "bad"
    elif i.name in EFU_LATENCY or i.name in ("WAITP", "MFP"):
        i.kind = "efu"
    elif i.name in ("LQI", "SQI", "LQD", "SQD", "ILWR", "ISWR"):
        i.kind = "mem"
    elif i.name == "MOVE" and i.dest == 0:
        i.kind = "nop"
    return i


class Pair:
    __slots__ = ("pc", "lo_word", "up_word", "lower", "upper", "ibit", "ebit",
                 "mbit", "dbit", "tbit")

    def __init__(self, pc, lo_word, up_word, pcmask=0x3FF8):
        self.pc = pc
        self.lo_word = lo_word
        self.up_word = up_word
        self.ibit = bool(up_word & 0x80000000)
        self.ebit = bool(up_word & 0x40000000)
        self.mbit = bool(up_word & 0x20000000)
        self.dbit = bool(up_word & 0x10000000)
        self.tbit = bool(up_word & 0x08000000)
        self.upper = decode_upper(up_word & 0x07FFFFFF)
        self.lower = None if self.ibit else decode_lower(lo_word, pc + 8, pcmask)

    @property
    def immediate(self):
        return self.lo_word if self.ibit else None


def decode_image(image, lo=0, hi=None):
    if hi is None:
        hi = len(image)
    pcmask = len(image) - 8
    out = {}
    for pc in range(lo, min(hi, len(image) - 7), 8):
        lo_word = int.from_bytes(image[pc:pc + 4], "little")
        up_word = int.from_bytes(image[pc + 4:pc + 8], "little")
        out[pc] = Pair(pc, lo_word, up_word, pcmask)
    return out
