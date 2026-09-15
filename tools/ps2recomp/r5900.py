from dataclasses import dataclass

CAT_ALU = "alu"
CAT_BRANCH = "branch"
CAT_BRANCH_LIKELY = "branchl"
CAT_JUMP = "jump"
CAT_JUMPR = "jumpr"
CAT_LOAD = "load"
CAT_STORE = "store"
CAT_TRAP = "trap"
CAT_SYSCALL = "syscall"
CAT_COP = "cop"
CAT_INVALID = "invalid"

GPR_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


@dataclass
class Insn:
    addr: int
    word: int
    name: str = "?"
    cat: str = CAT_INVALID
    op: int = 0
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    funct: int = 0
    imm: int = 0
    simm: int = 0
    target: int = 0
    btarget: int = 0
    fs: int = 0
    ft: int = 0
    fd: int = 0
    dest: int = 0
    bc: int = 0
    is_nop: bool = False
    writes_ra: bool = False
    raw_sub: int = 0

    @property
    def has_delay_slot(self) -> bool:
        return self.cat in (CAT_BRANCH, CAT_BRANCH_LIKELY, CAT_JUMP, CAT_JUMPR)

    @property
    def is_likely(self) -> bool:
        return self.cat == CAT_BRANCH_LIKELY

    def __str__(self):
        return "%08x: %08x %s" % (self.addr, self.word, self.name)


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


SPECIAL = {
    0x00: "SLL", 0x02: "SRL", 0x03: "SRA", 0x04: "SLLV", 0x06: "SRLV",
    0x07: "SRAV", 0x08: "JR", 0x09: "JALR", 0x0A: "MOVZ", 0x0B: "MOVN",
    0x0C: "SYSCALL", 0x0D: "BREAK", 0x0F: "SYNC",
    0x10: "MFHI", 0x11: "MTHI", 0x12: "MFLO", 0x13: "MTLO",
    0x14: "DSLLV", 0x16: "DSRLV", 0x17: "DSRAV",
    0x18: "MULT", 0x19: "MULTU", 0x1A: "DIV", 0x1B: "DIVU",
    0x20: "ADD", 0x21: "ADDU", 0x22: "SUB", 0x23: "SUBU",
    0x24: "AND", 0x25: "OR", 0x26: "XOR", 0x27: "NOR",
    0x28: "MFSA", 0x29: "MTSA",
    0x2A: "SLT", 0x2B: "SLTU", 0x2C: "DADD", 0x2D: "DADDU",
    0x2E: "DSUB", 0x2F: "DSUBU",
    0x30: "TGE", 0x31: "TGEU", 0x32: "TLT", 0x33: "TLTU",
    0x34: "TEQ", 0x36: "TNE",
    0x38: "DSLL", 0x3A: "DSRL", 0x3B: "DSRA",
    0x3C: "DSLL32", 0x3E: "DSRL32", 0x3F: "DSRA32",
}

REGIMM = {
    0x00: "BLTZ", 0x01: "BGEZ", 0x02: "BLTZL", 0x03: "BGEZL",
    0x08: "TGEI", 0x09: "TGEIU", 0x0A: "TLTI", 0x0B: "TLTIU",
    0x0C: "TEQI", 0x0E: "TNEI",
    0x10: "BLTZAL", 0x11: "BGEZAL", 0x12: "BLTZALL", 0x13: "BGEZALL",
    0x18: "MTSAB", 0x19: "MTSAH",
}

OPCODE = {
    0x02: "J", 0x03: "JAL", 0x04: "BEQ", 0x05: "BNE", 0x06: "BLEZ",
    0x07: "BGTZ", 0x08: "ADDI", 0x09: "ADDIU", 0x0A: "SLTI",
    0x0B: "SLTIU", 0x0C: "ANDI", 0x0D: "ORI", 0x0E: "XORI", 0x0F: "LUI",
    0x14: "BEQL", 0x15: "BNEL", 0x16: "BLEZL", 0x17: "BGTZL",
    0x18: "DADDI", 0x19: "DADDIU", 0x1A: "LDL", 0x1B: "LDR",
    0x1E: "LQ", 0x1F: "SQ",
    0x20: "LB", 0x21: "LH", 0x22: "LWL", 0x23: "LW", 0x24: "LBU",
    0x25: "LHU", 0x26: "LWR", 0x27: "LWU",
    0x28: "SB", 0x29: "SH", 0x2A: "SWL", 0x2B: "SW", 0x2C: "SDL",
    0x2D: "SDR", 0x2E: "SWR", 0x2F: "CACHE",
    0x31: "LWC1", 0x33: "PREF", 0x36: "LQC2", 0x37: "LD",
    0x39: "SWC1", 0x3E: "SQC2", 0x3F: "SD",
}

MMI = {
    0x00: "MADD", 0x01: "MADDU", 0x04: "PLZCW",
    0x10: "MFHI1", 0x11: "MTHI1", 0x12: "MFLO1", 0x13: "MTLO1",
    0x18: "MULT1", 0x19: "MULTU1", 0x1A: "DIV1", 0x1B: "DIVU1",
    0x20: "MADD1", 0x21: "MADDU1",
    0x30: "PMFHL", 0x31: "PMTHL",
    0x34: "PSLLH", 0x36: "PSRLH", 0x37: "PSRAH",
    0x3C: "PSLLW", 0x3E: "PSRLW", 0x3F: "PSRAW",
}

MMI0 = {
    0x00: "PADDW", 0x01: "PSUBW", 0x02: "PCGTW", 0x03: "PMAXW",
    0x04: "PADDH", 0x05: "PSUBH", 0x06: "PCGTH", 0x07: "PMAXH",
    0x08: "PADDB", 0x09: "PSUBB", 0x0A: "PCGTB",
    0x10: "PADDSW", 0x11: "PSUBSW", 0x12: "PEXTLW", 0x13: "PPACW",
    0x14: "PADDSH", 0x15: "PSUBSH", 0x16: "PEXTLH", 0x17: "PPACH",
    0x18: "PADDSB", 0x19: "PSUBSB", 0x1A: "PEXTLB", 0x1B: "PPACB",
    0x1E: "PEXT5", 0x1F: "PPAC5",
}

MMI1 = {
    0x01: "PABSW", 0x02: "PCEQW", 0x03: "PMINW",
    0x04: "PADSBH", 0x05: "PABSH", 0x06: "PCEQH", 0x07: "PMINH",
    0x0A: "PCEQB",
    0x10: "PADDUW", 0x11: "PSUBUW", 0x12: "PEXTUW",
    0x14: "PADDUH", 0x15: "PSUBUH", 0x16: "PEXTUH",
    0x18: "PADDUB", 0x19: "PSUBUB", 0x1A: "PEXTUB", 0x1B: "QFSRV",
}

MMI2 = {
    0x00: "PMADDW", 0x02: "PSLLVW", 0x03: "PSRLVW",
    0x04: "PMSUBW", 0x08: "PMFHI", 0x09: "PMFLO", 0x0A: "PINTH",
    0x0C: "PMULTW", 0x0D: "PDIVW", 0x0E: "PCPYLD",
    0x10: "PMADDH", 0x11: "PHMADH", 0x12: "PAND", 0x13: "PXOR",
    0x14: "PMSUBH", 0x15: "PHMSBH",
    0x1A: "PEXEH", 0x1B: "PREVH", 0x1C: "PMULTH", 0x1D: "PDIVBW",
    0x1E: "PEXEW", 0x1F: "PROT3W",
}

MMI3 = {
    0x00: "PMADDUW", 0x03: "PSRAVW",
    0x08: "PMTHI", 0x09: "PMTLO", 0x0A: "PINTEH",
    0x0C: "PMULTUW", 0x0D: "PDIVUW", 0x0E: "PCPYUD",
    0x12: "POR", 0x13: "PNOR",
    0x1A: "PEXCH", 0x1B: "PCPYH", 0x1E: "PEXCW",
}

COP1_S = {
    0x00: "ADD.S", 0x01: "SUB.S", 0x02: "MUL.S", 0x03: "DIV.S",
    0x04: "SQRT.S", 0x05: "ABS.S", 0x06: "MOV.S", 0x07: "NEG.S",
    0x16: "RSQRT.S", 0x18: "ADDA.S", 0x19: "SUBA.S", 0x1A: "MULA.S",
    0x1C: "MADD.S", 0x1D: "MSUB.S", 0x1E: "MADDA.S", 0x1F: "MSUBA.S",
    0x24: "CVT.W.S", 0x28: "MAX.S", 0x29: "MIN.S",
    0x30: "C.F.S", 0x32: "C.EQ.S", 0x34: "C.LT.S", 0x36: "C.LE.S",
}
COP1_W = {0x20: "CVT.S.W"}

VU_BC = ["x", "y", "z", "w"]

COP2_SPECIAL1 = {}
for _i, _n in enumerate(("VADD", "VSUB", "VMADD", "VMSUB", "VMAX", "VMINI",
                         "VMUL")):
    for _b in range(4):
        COP2_SPECIAL1[_i * 4 + _b] = _n + VU_BC[_b]
COP2_SPECIAL1.update({
    0x1C: "VMULq", 0x1D: "VMAXi", 0x1E: "VMULi", 0x1F: "VMINIi",
    0x20: "VADDq", 0x21: "VMADDq", 0x22: "VADDi", 0x23: "VMADDi",
    0x24: "VSUBq", 0x25: "VMSUBq", 0x26: "VSUBi", 0x27: "VMSUBi",
    0x28: "VADD", 0x29: "VMADD", 0x2A: "VMUL", 0x2B: "VMAX",
    0x2C: "VSUB", 0x2D: "VMSUB", 0x2E: "VOPMSUB", 0x2F: "VMINI",
    0x30: "VIADD", 0x31: "VISUB", 0x32: "VIADDI",
    0x34: "VIAND", 0x35: "VIOR",
    0x38: "VCALLMS", 0x39: "VCALLMSR",
})

COP2_SPECIAL2 = {}
for _i, _n in enumerate(("VADDA", "VSUBA", "VMADDA", "VMSUBA")):
    for _b in range(4):
        COP2_SPECIAL2[_i * 4 + _b] = _n + VU_BC[_b]
COP2_SPECIAL2.update({
    0x10: "VITOF0", 0x11: "VITOF4", 0x12: "VITOF12", 0x13: "VITOF15",
    0x14: "VFTOI0", 0x15: "VFTOI4", 0x16: "VFTOI12", 0x17: "VFTOI15",
    0x18: "VMULAx", 0x19: "VMULAy", 0x1A: "VMULAz", 0x1B: "VMULAw",
    0x1C: "VMULAq", 0x1D: "VABS", 0x1E: "VMULAi", 0x1F: "VCLIPw",
    0x20: "VADDAq", 0x21: "VMADDAq", 0x22: "VADDAi", 0x23: "VMADDAi",
    0x24: "VSUBAq", 0x25: "VMSUBAq", 0x26: "VSUBAi", 0x27: "VMSUBAi",
    0x28: "VADDA", 0x29: "VMADDA", 0x2A: "VMULA",
    0x2C: "VSUBA", 0x2D: "VMSUBA", 0x2E: "VOPMULA", 0x2F: "VNOP",
    0x30: "VMOVE", 0x31: "VMR32",
    0x34: "VLQI", 0x35: "VSQI", 0x36: "VLQD", 0x37: "VSQD",
    0x38: "VDIV", 0x39: "VSQRT", 0x3A: "VRSQRT", 0x3B: "VWAITQ",
    0x3C: "VMTIR", 0x3D: "VMFIR", 0x3E: "VILWR", 0x3F: "VISWR",
    0x40: "VRNEXT", 0x41: "VRGET", 0x42: "VRINIT", 0x43: "VRXOR",
})

COP0_C0 = {
    0x01: "TLBR", 0x02: "TLBWI", 0x06: "TLBWR", 0x08: "TLBP",
    0x18: "ERET", 0x38: "EI", 0x39: "DI",
}

LOADS = {"LB", "LBU", "LH", "LHU", "LW", "LWU", "LWL", "LWR", "LD", "LDL",
         "LDR", "LQ", "LWC1", "LQC2"}
STORES = {"SB", "SH", "SW", "SWL", "SWR", "SD", "SDL", "SDR", "SQ", "SWC1",
          "SQC2"}
BRANCHES = {"BEQ", "BNE", "BLEZ", "BGTZ", "BLTZ", "BGEZ", "BLTZAL", "BGEZAL"}
BRANCHES_L = {"BEQL", "BNEL", "BLEZL", "BGTZL", "BLTZL", "BGEZL",
              "BLTZALL", "BGEZALL"}
TRAPS = {"TGE", "TGEU", "TLT", "TLTU", "TEQ", "TNE", "TGEI", "TGEIU",
         "TLTI", "TLTIU", "TEQI", "TNEI", "BREAK"}


def decode(word: int, addr: int) -> Insn:
    i = Insn(addr=addr, word=word)
    i.op = op = (word >> 26) & 0x3F
    i.rs = rs = (word >> 21) & 0x1F
    i.rt = rt = (word >> 16) & 0x1F
    i.rd = rd = (word >> 11) & 0x1F
    i.sa = sa = (word >> 6) & 0x1F
    i.funct = funct = word & 0x3F
    i.imm = word & 0xFFFF
    i.simm = _s16(i.imm)
    i.fs, i.ft, i.fd = rd, rt, sa
    i.dest = (word >> 21) & 0xF
    i.bc = word & 0x3
    i.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    i.btarget = (addr + 4 + (i.simm << 2)) & 0xFFFFFFFF

    if word == 0:
        i.name, i.cat, i.is_nop = "NOP", CAT_ALU, True
        return i

    if op == 0x00:
        nm = SPECIAL.get(funct)
        if nm is None:
            i.name, i.cat = "SPECIAL_%02x" % funct, CAT_INVALID
            return i
        i.name = nm
        if nm == "JR":
            i.cat = CAT_JUMPR
        elif nm == "JALR":
            i.cat, i.writes_ra = CAT_JUMPR, (rd == 31)
        elif nm == "SYSCALL":
            i.cat = CAT_SYSCALL
        elif nm in TRAPS:
            i.cat = CAT_TRAP
        elif nm == "SYNC":
            i.cat, i.is_nop = CAT_ALU, True
        else:
            i.cat = CAT_ALU
        return i

    if op == 0x01:
        nm = REGIMM.get(rt)
        if nm is None:
            i.name, i.cat = "REGIMM_%02x" % rt, CAT_INVALID
            return i
        i.name = nm
        if nm in BRANCHES:
            i.cat = CAT_BRANCH
        elif nm in BRANCHES_L:
            i.cat = CAT_BRANCH_LIKELY
        elif nm in TRAPS:
            i.cat = CAT_TRAP
        else:
            i.cat = CAT_ALU
        i.writes_ra = nm.endswith("AL") or nm.endswith("ALL")
        return i

    if op == 0x10:
        i.cat = CAT_COP
        if rs == 0x00:
            i.name = "MFC0"
        elif rs == 0x04:
            i.name = "MTC0"
        elif rs == 0x08:
            i.name = ("BC0F", "BC0T", "BC0FL", "BC0TL")[rt & 3]
            i.cat = CAT_BRANCH_LIKELY if rt & 2 else CAT_BRANCH
        elif rs & 0x10:
            i.name = COP0_C0.get(funct, "C0_%02x" % funct)
            if i.name.startswith("C0_"):
                i.cat = CAT_INVALID
        else:
            i.name, i.cat = "COP0_%02x" % rs, CAT_INVALID
        return i

    if op == 0x11:
        i.cat = CAT_COP
        if rs == 0x00:
            i.name = "MFC1"
        elif rs == 0x02:
            i.name = "CFC1"
        elif rs == 0x04:
            i.name = "MTC1"
        elif rs == 0x06:
            i.name = "CTC1"
        elif rs == 0x08:
            i.name = ("BC1F", "BC1T", "BC1FL", "BC1TL")[rt & 3]
            i.cat = CAT_BRANCH_LIKELY if rt & 2 else CAT_BRANCH
        elif rs == 0x10:
            i.name = COP1_S.get(funct, "COP1S_%02x" % funct)
            if i.name.startswith("COP1S_"):
                i.cat = CAT_INVALID
        elif rs == 0x14:
            i.name = COP1_W.get(funct, "COP1W_%02x" % funct)
            if i.name.startswith("COP1W_"):
                i.cat = CAT_INVALID
        else:
            i.name, i.cat = "COP1_%02x" % rs, CAT_INVALID
        return i

    if op == 0x12:
        i.cat = CAT_COP
        if rs == 0x01:
            i.name = "QMFC2"
        elif rs == 0x02:
            i.name = "CFC2"
        elif rs == 0x05:
            i.name = "QMTC2"
        elif rs == 0x06:
            i.name = "CTC2"
        elif rs == 0x08:
            i.name = ("BC2F", "BC2T", "BC2FL", "BC2TL")[rt & 3]
            i.cat = CAT_BRANCH_LIKELY if rt & 2 else CAT_BRANCH
        elif rs & 0x10:
            if (funct & 0x3C) == 0x3C:
                sub = (word & 0x3) | ((word >> 4) & 0x7C)
                i.raw_sub = sub
                i.name = COP2_SPECIAL2.get(sub, "VSPEC2_%02x" % sub)
                if i.name.startswith("VSPEC2_"):
                    i.cat = CAT_INVALID
            else:
                i.raw_sub = funct
                i.name = COP2_SPECIAL1.get(funct, "VSPEC1_%02x" % funct)
                if i.name.startswith("VSPEC1_"):
                    i.cat = CAT_INVALID
        else:
            i.name, i.cat = "COP2_%02x" % rs, CAT_INVALID
        return i

    if op == 0x1C:
        nm = MMI.get(funct)
        if nm is None:
            if funct == 0x08:
                nm = MMI0.get(sa)
            elif funct == 0x09:
                nm = MMI2.get(sa)
            elif funct == 0x28:
                nm = MMI1.get(sa)
            elif funct == 0x29:
                nm = MMI3.get(sa)
        if nm is None:
            i.name, i.cat = "MMI_%02x_%02x" % (funct, sa), CAT_INVALID
            return i
        i.name, i.cat = nm, CAT_ALU
        return i

    nm = OPCODE.get(op)
    if nm is None:
        i.name, i.cat = "OP_%02x" % op, CAT_INVALID
        return i
    i.name = nm
    if nm == "J":
        i.cat = CAT_JUMP
    elif nm == "JAL":
        i.cat, i.writes_ra = CAT_JUMP, True
    elif nm in BRANCHES:
        i.cat = CAT_BRANCH
    elif nm in BRANCHES_L:
        i.cat = CAT_BRANCH_LIKELY
    elif nm in LOADS:
        i.cat = CAT_LOAD
    elif nm in STORES:
        i.cat = CAT_STORE
    elif nm in ("CACHE", "PREF"):
        i.cat, i.is_nop = CAT_ALU, True
    else:
        i.cat = CAT_ALU
    return i
