from vurecomp import vudec


class Block:
    __slots__ = ("start", "pairs", "succs", "ends_program", "indirect")

    def __init__(self, start):
        self.start = start
        self.pairs = []
        self.succs = []
        self.ends_program = False
        self.indirect = False

    def __repr__(self):
        return "Block(%04X, %d pairs, -> %s%s)" % (
            self.start, len(self.pairs),
            [("%04X" % s) for s in self.succs],
            " [end]" if self.ends_program else "")


class Cfg:
    def __init__(self):
        self.blocks = {}
        self.entries = []
        self.reachable_pairs = 0
        self.unreached = []
        self.indirect_blocks = []
        self.call_returns = []


def build_cfg(code, entries):
    cfg = Cfg()
    cfg.entries = list(entries)
    if not code:
        return cfg

    lo = min(code)
    hi = max(code) + 8

    leaders = set(e for e in entries if e in code)
    for pc, pair in code.items():
        l = pair.lower
        if l is not None and l.kind == "branch" and l.target is not None:
            if lo <= l.target < hi:
                leaders.add(l.target)
        if (l is not None and l.kind in ("branch", "jump")) or pair.ebit:
            after = pc + 16
            if after in code:
                leaders.add(after)

    for pc, pair in code.items():
        l = pair.lower
        if l is not None and l.name == "BAL":
            ret = pc + 16
            if ret in code:
                leaders.add(ret)
                cfg.call_returns.append(ret)

    work = [e for e in entries if e in code] + list(cfg.call_returns)
    seen = set()
    while work:
        start = work.pop()
        if start in seen:
            continue
        seen.add(start)
        blk = Block(start)
        cfg.blocks[start] = blk

        pc = start
        while pc in code:
            pair = code[pc]
            blk.pairs.append(pc)
            l = pair.lower

            if pair.ebit:
                slot = pc + 8
                if slot in code:
                    blk.pairs.append(slot)
                blk.ends_program = True
                break
            if l is not None and l.kind in ("branch", "jump"):
                slot = pc + 8
                if slot in code:
                    blk.pairs.append(slot)
                    if code[slot].ebit:
                        blk.ends_program = True
                if l.kind == "jump":
                    blk.indirect = True
                    cfg.indirect_blocks.append(start)
                elif not blk.ends_program:
                    if l.target is not None and l.target in code:
                        blk.succs.append(l.target)
                    if l.name != "B" and l.name != "BAL":
                        fall = pc + 16
                        if fall in code:
                            blk.succs.append(fall)
                break

            nxt = pc + 8
            if nxt in leaders:
                if nxt in code:
                    blk.succs.append(nxt)
                break
            pc = nxt
        else:
            blk.ends_program = True

        for s in blk.succs:
            if s not in seen:
                work.append(s)

    covered = set()
    for blk in cfg.blocks.values():
        covered.update(blk.pairs)
    cfg.reachable_pairs = len(covered)
    cfg.unreached = sorted(set(code) - covered)
    return cfg
