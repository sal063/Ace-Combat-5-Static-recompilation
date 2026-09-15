import sys, os, json, time
sys.path.insert(0, os.path.dirname(__file__))
from ps2recomp.elf import ElfFile
from ps2recomp.analysis import Program
from ps2recomp import sigmatch

import paths

GAME = paths.game()
IDA = paths.config("ida_db.json")
SEEDS = paths.config("ida_seeds.json")
LIBS = paths.sdk_libs()
OUT = paths.config("sdk_symbols.json")

t0 = time.time()
elf = ElfFile(GAME)
prog = Program(elf, IDA, log=lambda *a: None)
prog.seed()
with open(SEEDS) as fp:
    seeds = json.load(fp)
for key in ("xref", "ptr"):
    for a in seeds.get(key, ()):
        if prog.in_text(a):
            prog.entries.add(a)

prog.partition()

sigs = sigmatch.build_signatures(LIBS)
found = sigmatch.match(sigs, prog)
found = sigmatch.anchor_object_symbols(LIBS, prog, found)
found = sigmatch.align_object_symbols(LIBS, prog, found)

by_lib = {}
for a, (name, lib, n) in found.items():
    by_lib.setdefault(lib, []).append((a, name, n))
print("--- by library ---")
for lib in sorted(by_lib, key=lambda k: -len(by_lib[k])):
    print("  %-28s %d" % (lib, len(by_lib[lib])))

sif = sorted((a, n) for a, (n, l, c) in found.items() if n.startswith("sceSif")
             or n.startswith("_sceSif") or n.startswith("sceCd"))
print("--- SIF / CD entry points (%d) ---" % len(sif))
for a, n in sif:
    print("  %08X  %s" % (a, n))

MANUAL = paths.config("manual_symbols.json")
if os.path.exists(MANUAL):
    man = json.load(open(MANUAL))
    nman = 0
    for k, v in man.items():
        if k.startswith("//"):
            continue
        a = int(k, 16)
        found[a] = (v["name"], v.get("src", "manual"), 0)
        nman += 1
    print("merged %d hand-derived symbols from %s" % (nman, MANUAL))

json.dump({("0x%08X" % a): dict(name=n, lib=l, words=c)
           for a, (n, l, c) in sorted(found.items())},
          open(OUT, "w"), indent=1)
print("wrote %s (%d symbols) in %.1fs" % (OUT, len(found), time.time() - t0))
