import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2recomp.r5900 import decode

LINE = re.compile(r"^\s*([0-9a-f]+):	([0-9a-f]{8}) 	(\S+)")


def load(disasm_dir):
    seen = {}
    for fn in sorted(os.listdir(disasm_dir)):
        if not fn.endswith(".txt"):
            continue
        with open(os.path.join(disasm_dir, fn), errors="replace") as fp:
            for line in fp:
                m = LINE.match(line)
                if not m:
                    continue
                addr, word, mnem = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
                if word not in seen:
                    seen[word] = (mnem, addr, fn)
    return seen


ALIASES = {
    "nop":    lambda i, w: w == 0,
    "ssnop":  lambda i, w: w == 0x40,
    "b":      lambda i, w: i.name == "BEQ" and i.rs == 0 and i.rt == 0,
    "bal":    lambda i, w: i.name == "BGEZAL" and i.rs == 0,
    "beqz":   lambda i, w: i.name == "BEQ" and i.rt == 0,
    "bnez":   lambda i, w: i.name == "BNE" and i.rt == 0,
    "beqzl":  lambda i, w: i.name == "BEQL" and i.rt == 0,
    "bnezl":  lambda i, w: i.name == "BNEL" and i.rt == 0,
    "li":     lambda i, w: i.name in ("ADDIU", "ORI", "DADDIU") and i.rs == 0,
    "move":   lambda i, w: i.name in ("ADDU", "OR", "DADDU", "DADD") and (i.rt == 0 or i.rs == 0),
    "neg":    lambda i, w: i.name == "SUB" and i.rs == 0,
    "negu":   lambda i, w: i.name == "SUBU" and i.rs == 0,
    "dneg":   lambda i, w: i.name == "DSUB" and i.rs == 0,
    "dnegu":  lambda i, w: i.name == "DSUBU" and i.rs == 0,
    "not":    lambda i, w: i.name == "NOR" and (i.rt == 0 or i.rs == 0),
}


VU_FIELD = {"x": 8, "y": 4, "z": 2, "w": 1}


def dest_mask(mnem):
    if not mnem.startswith("v") or "." not in mnem:
        return None
    suffix = mnem.rsplit(".", 1)[1]
    if not suffix or any(c not in VU_FIELD for c in suffix):
        return None
    m = 0
    for c in suffix:
        m |= VU_FIELD[c]
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disasm", default="sdk_corpus/_disasm")
    ap.add_argument("--raw", action="store_true",
                    help="do not apply the alias table; show every difference")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    seen = load(args.disasm)
    print("distinct instruction words: %d" % len(seen))

    buckets = collections.defaultdict(list)
    agree = 0
    for word, (mnem, addr, fn) in seen.items():
        ins = decode(word, addr)
        ours = ins.name.lower()
        theirs = mnem.lower()
        if ours == theirs:
            agree += 1
            continue
        if not args.raw:
            rule = ALIASES.get(theirs)
            if rule and rule(ins, word):
                agree += 1
                continue
            dm = dest_mask(theirs)
            if dm is not None and theirs.rsplit(".", 1)[0] == ours:
                if dm == ins.dest:
                    agree += 1
                    continue
                buckets[("dest-mask MISMATCH", ours)].append((word, addr, fn))
                continue
        buckets[(theirs, ours)].append((word, addr, fn))

    diff = sum(len(v) for v in buckets.values())
    print("agree: %d   differ: %d   (%.3f%% agreement)"
          % (agree, diff, 100.0 * agree / max(1, len(seen))))
    print()
    print("%-14s %-14s %8s   %s" % ("objdump", "r5900.py", "count", "example"))
    print("-" * 78)
    for (theirs, ours), v in sorted(buckets.items(), key=lambda kv: -len(kv[1]))[:args.top]:
        w, a, fn = v[0]
        print("%-14s %-14s %8d   %08x at %08x in %s"
              % (theirs, ours, len(v), w, a, fn[:28]))
    if len(buckets) > args.top:
        print("... and %d more buckets" % (len(buckets) - args.top))


if __name__ == "__main__":
    main()
