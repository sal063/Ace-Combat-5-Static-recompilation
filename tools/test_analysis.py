import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from ps2recomp.elf import ElfFile
from ps2recomp.analysis import Program

import paths

GAME = paths.game()
IDA = paths.config("ida_db.json")

t0 = time.time()
elf = ElfFile(GAME)
p = Program(elf, IDA)
print("prefetch %.1fs" % (time.time() - t0))
p.seed()
t1 = time.time()
p.partition()
print("partition %.1fs" % (time.time() - t1))

unc = p.uncovered_ranges()
tot = sum(b - a for a, b in unc)
print("uncovered: %d ranges, %d bytes" % (len(unc), tot))
unc.sort(key=lambda r: r[1] - r[0], reverse=True)
for a, b in unc[:12]:
    print("   %08x..%08x  %5d bytes" % (a, b, b - a))

bad = p.pathological_delay_slots()
print("delay slots that are also branch targets: %d" % len(bad))
for f, a in bad[:10]:
    print("   fn %08x  ds %08x" % (f, a))

ind = sum(len(f.indirect_jumps) for f in p.functions.values())
print("unresolved indirect jumps (jr, not $ra, no switch): %d" % ind)
sizes = sorted((f.size for f in p.functions.values()), reverse=True)
print("largest functions (bytes):", sizes[:8])
print("total emitted instructions:", sum(len(f.addrs) for f in p.functions.values()))
print("tail calls:", sum(len(f.tail_calls) for f in p.functions.values()))
