import json
from dataclasses import dataclass, field
from typing import Dict, List, Set, Tuple

from . import r5900
from .r5900 import (CAT_BRANCH, CAT_BRANCH_LIKELY, CAT_JUMP, CAT_JUMPR,
                    CAT_SYSCALL, CAT_TRAP, decode)


@dataclass
class Function:
    entry: int
    name: str
    addrs: List[int] = field(default_factory=list)
    labels: Set[int] = field(default_factory=set)
    delay_slots: Set[int] = field(default_factory=set)
    tail_calls: Set[int] = field(default_factory=set)
    calls: Set[int] = field(default_factory=set)
    indirect_jumps: List[int] = field(default_factory=list)
    noret: bool = False

    @property
    def size(self) -> int:
        return len(self.addrs) * 4


class Program:
    def __init__(self, elf, ida_db_path=None, log=print):
        self.elf = elf
        self.log = log
        self.text = elf.text_ranges()
        self.lo = min(a for a, _ in self.text)
        self.hi = max(b for _, b in self.text)
        self.words: Dict[int, int] = {}
        self.insns: Dict[int, r5900.Insn] = {}
        self.names: Dict[int, str] = {}
        self.switches: Dict[int, List[int]] = {}
        self.ida_funcs: List[dict] = []
        self.entries: Set[int] = set()
        self.owner: Dict[int, int] = {}
        self.functions: Dict[int, Function] = {}
        self.stats = {}
        self._prefetch()
        if ida_db_path:
            self._load_ida(ida_db_path)

    def in_text(self, a: int) -> bool:
        return any(lo <= a < hi for lo, hi in self.text)

    def _prefetch(self):
        elf = self.elf
        for lo, hi in self.text:
            for a in range(lo, hi, 4):
                w = elf.word(a)
                self.words[a] = w
                self.insns[a] = decode(w, a)

    def _load_ida(self, path):
        with open(path) as fp:
            db = json.load(fp)
        self.ida_funcs = db["functions"]
        for k, v in db["names"].items():
            self.names[int(k)] = v
        for sw in db["switches"]:
            tg = [t for t in sw["targets"] if self.in_text(t)]
            if tg:
                self.switches.setdefault(sw["ea"], []).extend(tg)
        for a in list(self.switches):
            self.switches[a] = sorted(set(self.switches[a]))

    def seed(self):
        e = self.entries
        if self.in_text(self.elf.entry):
            e.add(self.elf.entry)

        for f in self.ida_funcs:
            if self.in_text(f["ea"]):
                e.add(f["ea"])

        jal_targets = 0
        for a, ins in self.insns.items():
            if ins.name == "JAL" and self.in_text(ins.target):
                if ins.target not in e:
                    jal_targets += 1
                e.add(ins.target)

        sw_targets = 0
        for tg in self.switches.values():
            for t in tg:
                if t not in e:
                    sw_targets += 1
                e.add(t)

        ctor = 0
        for sec in (".ctors", ".dtors"):
            s = self.elf.section(sec)
            if not s or not s.data:
                continue
            for i in range(0, len(s.data) - 3, 4):
                v = int.from_bytes(s.data[i:i + 4], "little")
                if self.in_text(v) and v & 3 == 0:
                    if v not in e:
                        ctor += 1
                    e.add(v)

        table = 0
        table_addrs = []
        for sec in self.elf.sections:
            data = getattr(sec, "data", None)
            if not data or not getattr(sec, "addr", 0):
                continue
            if sec.name in (".ctors", ".dtors"):
                continue
            if sec.name.startswith(".text") or sec.name == ".vutext":
                continue
            for i in range(0, len(data) - 3, 4):
                v = int.from_bytes(data[i:i + 4], "little")
                if v & 3 or not self.in_text(v) or v in e:
                    continue
                ins = self.insns.get(v)
                if ins is None or ins.name != "ADDIU":
                    continue
                if ins.rs != 29 or ins.rt != 29 or ins.simm >= 0:
                    continue
                e.add(v)
                table_addrs.append(v)
                table += 1

        self.stats["seed_ida"] = len(self.ida_funcs)
        self.stats["seed_jal_extra"] = jal_targets
        self.stats["seed_switch_extra"] = sw_targets
        self.stats["seed_ctor_extra"] = ctor
        self.stats["seed_table_extra"] = table
        self.stats["seed_table_addrs"] = ["%08X" % a for a in sorted(table_addrs)]
        self.log("seed: %d entries (ida=%d, +jal=%d, +switch=%d, +ctors=%d,"
                 " +tables=%d)"
                 % (len(e), len(self.ida_funcs), jal_targets, sw_targets, ctor,
                    table))
        if table_addrs:
            self.log("seed: handler-table entries IDA did not type as "
                     "functions: %s"
                     % " ".join("%08X" % a for a in sorted(table_addrs)))

    def _successors(self, ins, fn: Function):
        a = ins.addr
        cat = ins.cat
        if cat == CAT_BRANCH or cat == CAT_BRANCH_LIKELY:
            return [ins.btarget, a + 8], True, True
        if cat == CAT_JUMP:
            if ins.name == "JAL":
                fn.calls.add(ins.target)
                return [a + 8], True, True
            return [ins.target], True, False
        if cat == CAT_JUMPR:
            if ins.name == "JALR":
                return [a + 8], True, True
            if ins.rs == 31:
                return [], True, False
            tg = self.switches.get(a)
            if tg:
                return list(tg), True, False
            fn.indirect_jumps.append(a)
            return [], True, False
        return [a + 4], False, True

    def partition(self):
        rounds = 0
        while True:
            rounds += 1
            self.owner = {}
            self.functions = {}
            promote: Set[int] = set()
            for entry in sorted(self.entries):
                if not self.in_text(entry):
                    continue
                fn = Function(entry=entry,
                              name=self.names.get(entry, "sub_%08X" % entry))
                self.functions[entry] = fn
                work = [entry]
                seen: Set[int] = set()
                while work:
                    a = work.pop()
                    if a in seen:
                        continue
                    if not self.in_text(a):
                        continue
                    own = self.owner.get(a)
                    if own is not None:
                        if own != entry:
                            fn.tail_calls.add(a)
                            if a not in self.entries:
                                promote.add(a)
                        continue
                    if a != entry and a in self.entries:
                        fn.tail_calls.add(a)
                        continue
                    seen.add(a)
                    self.owner[a] = entry
                    ins = self.insns[a]
                    succ, uses_ds, _ft = self._successors(ins, fn)
                    if uses_ds:
                        ds = a + 4
                        if self.in_text(ds):
                            fn.delay_slots.add(ds)
                            if self.owner.get(ds) is None:
                                self.owner[ds] = entry
                    for s in succ:
                        if not self.in_text(s):
                            fn.tail_calls.add(s)
                            continue
                        own_s = self.owner.get(s)
                        if own_s is not None and own_s != entry:
                            fn.tail_calls.add(s)
                            if s not in self.entries:
                                promote.add(s)
                            continue
                        if s != entry and s in self.entries:
                            fn.tail_calls.add(s)
                            continue
                        work.append(s)
                fn.addrs = sorted(seen)
            if not promote:
                break
            if rounds >= 64:
                raise ValueError("partition did not converge after %d rounds (%d unresolved)"
                                 % (rounds, len(promote)))
            self.entries |= promote
            self.log("partition round %d: promoted %d shared-code addresses"
                     % (rounds, len(promote)))
        self.stats["partition_rounds"] = rounds
        self._compute_labels()
        covered = len(self.owner) * 4
        total = sum(hi - lo for lo, hi in self.text)
        self.stats["functions"] = len(self.functions)
        self.stats["covered_bytes"] = covered
        self.stats["text_bytes"] = total
        self.stats["coverage"] = 100.0 * covered / total
        self.log("partition: %d functions, %d/%d bytes of .text covered (%.2f%%), "
                 "%d rounds"
                 % (len(self.functions), covered, total,
                    100.0 * covered / total, rounds))

    def _compute_labels(self):
        for fn in self.functions.values():
            own = set(fn.addrs)
            for a in fn.addrs:
                ins = self.insns[a]
                if ins.cat in (CAT_BRANCH, CAT_BRANCH_LIKELY):
                    if ins.btarget in own:
                        fn.labels.add(ins.btarget)
                elif ins.cat == CAT_JUMP and ins.name == "J":
                    if ins.target in own:
                        fn.labels.add(ins.target)
                elif ins.cat == CAT_JUMPR and ins.name == "JR":
                    for t in self.switches.get(a, ()):
                        if t in own:
                            fn.labels.add(t)

    def uncovered_ranges(self):
        out, run = [], None
        for lo, hi in self.text:
            for a in range(lo, hi, 4):
                if a in self.owner:
                    if run:
                        out.append(run)
                        run = None
                else:
                    if run and run[1] == a:
                        run = (run[0], a + 4)
                    else:
                        if run:
                            out.append(run)
                        run = (a, a + 4)
        if run:
            out.append(run)
        return out

    def pathological_delay_slots(self):
        bad = []
        targets = {i.btarget for i in self.insns.values()
                   if i.cat in (CAT_BRANCH, CAT_BRANCH_LIKELY)}
        targets.update(i.target for i in self.insns.values() if i.name == "J")
        targets.update(t for ts in self.switches.values() for t in ts)
        for fn in self.functions.values():
            for a in fn.delay_slots:
                if a in targets:
                    bad.append((fn.entry, a))
        return bad
