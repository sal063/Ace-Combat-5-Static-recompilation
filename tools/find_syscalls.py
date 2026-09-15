import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
from ps2recomp.elf import ElfFile
from ps2recomp.analysis import Program
from ps2recomp import r5900

import paths

GAME = paths.game()
IDA = paths.config("ida_db.json")
TAB = paths.config("ee_syscalls.json")

names = json.load(open(TAB))
elf = ElfFile(GAME)
p = Program(elf, IDA, log=lambda *a: None)
p.seed()
p.partition()

sites = []
for fn in p.functions.values():
    body = sorted(set(fn.addrs) | fn.delay_slots)
    for idx, a in enumerate(body):
        if p.insns[a].name != "SYSCALL":
            continue
        num = None
        for j in range(idx - 1, max(-1, idx - 12), -1):
            ins = p.insns[body[j]]
            if ins.name in ("ADDIU", "ADDI", "ORI", "DADDIU") and ins.rt == 3 \
                    and ins.rs == 0:
                num = ins.simm if ins.name != "ORI" else ins.imm
                break
            if ins.name == "LUI" and ins.rt == 3:
                num = ins.imm << 16
                break
            if ins.name in ("DADDU", "ADDU", "OR") and ins.rd == 3:
                break
        sites.append((fn.entry, a, num))

seen = {}
for fnaddr, a, num in sites:
    key = num if num is not None else "unknown"
    seen.setdefault(key, []).append((fnaddr, a))

print("%d syscall sites, %d distinct selectors" % (len(sites), len(seen)))
for k in sorted(seen, key=lambda v: (isinstance(v, str), v)):
    nm = names.get(str(k), ["?"]) if not isinstance(k, str) else ["?"]
    print("  %-6s %-34s x%-3d  first at %08X (fn %08X)"
          % (k, "/".join(nm), len(seen[k]), seen[k][0][1], seen[k][0][0]))
out = {str(k): dict(names=names.get(str(k), []),
                    sites=["%08X" % a for _, a in v])
       for k, v in seen.items() if not isinstance(k, str)}
json.dump(out, open(paths.config("ac5_syscalls_used.json"), "w"),
          indent=1)
