import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
from ps2recomp.elf import ElfFile
from ps2recomp.analysis import Program
from ps2recomp import r5900
from collections import Counter

import paths

GAME = paths.game()
IDA = paths.config("ida_db.json")
elf = ElfFile(GAME)
p = Program(elf, IDA, log=lambda *a: None)
p.seed()
p.partition()

c = Counter()
for fn in p.functions.values():
    for a in fn.addrs:
        c[p.insns[a].name] += 1
    for a in fn.delay_slots:
        c[p.insns[a].name] += 1

MMI = set()
for d in (r5900.MMI, r5900.MMI0, r5900.MMI1, r5900.MMI2, r5900.MMI3):
    MMI |= set(d.values())
VU = set(r5900.COP2_SPECIAL1.values()) | set(r5900.COP2_SPECIAL2.values()) | {
    "QMFC2", "QMTC2", "CFC2", "CTC2", "LQC2", "SQC2"}
FPU = set(r5900.COP1_S.values()) | set(r5900.COP1_W.values()) | {
    "MFC1", "MTC1", "CFC1", "CTC1", "LWC1", "SWC1", "BC1T", "BC1F", "BC1TL", "BC1FL"}

for title, grp in (("MMI", MMI), ("VU0-macro", VU), ("COP1", FPU)):
    used = [(n, c[n]) for n in grp if c[n]]
    used.sort(key=lambda kv: -kv[1])
    print("== %s: %d distinct opcodes used, %d instructions" %
          (title, len(used), sum(v for _, v in used)))
    print("   ", used)
    unused = sorted(n for n in grp if not c[n])
    print("    unused:", unused)
print("== COP0:", [(n, c[n]) for n in ("MFC0", "MTC0", "EI", "DI", "ERET",
                                       "TLBR", "TLBWI", "TLBWR", "TLBP") if c[n]])
print("== SYSCALL:", c["SYSCALL"], " BREAK:", c["BREAK"])
print("== total emitted:", sum(c.values()))
