import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from ps2recomp.elf import ElfFile
from ps2recomp import r5900
from collections import Counter

import paths

GAME = paths.game()
e = ElfFile(GAME)
lo, hi = e.text_ranges()[0]
c = Counter()
bad = Counter()
n = 0
for a in range(lo, hi, 4):
    i = r5900.decode(e.word(a), a)
    n += 1
    c[i.name] += 1
    if i.cat == r5900.CAT_INVALID:
        bad[i.name] += 1
print("decoded %d instructions over %08x..%08x" % (n, lo, hi))
print("invalid: %d (%.4f%%)" % (sum(bad.values()), 100.0 * sum(bad.values()) / n))
print("top 30 :", c.most_common(30))
print("invalid:", bad.most_common(25))
mmi = sum(v for k, v in c.items() if k.startswith("P") or k.endswith("1"))
vu = sum(v for k, v in c.items() if k.startswith("V") or k[1:] in ("MFC2", "MTC2"))
fpu = sum(v for k, v in c.items() if k.endswith(".S") or k.endswith(".W") or k in ("MFC1", "MTC1", "CFC1", "CTC1", "LWC1", "SWC1"))
print("MMI-ish=%d  VU-macro-ish=%d  FPU-ish=%d" % (mmi, vu, fpu))
