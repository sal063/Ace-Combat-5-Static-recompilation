import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2recomp.elf import ElfFile
import paths

VU_C = "runtime/src/ps2_vu.c"
GAME = paths.GAME

SWITCHES = {
    "upper.sub": ("vu_upper", "sub"),
    "upper.funct": ("vu_upper", "funct"),
    "lower.op": ("vu_lower", "op"),
    "lower.low6": ("vu_lower", "low6"),
    "lower.sub": ("vu_lower", "sub"),
}
CASE = re.compile(r"case\s+(0x[0-9A-Fa-f]+)\s*:")


def handled(path):
    source = open(path, encoding="utf-8").read()
    source = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', ' ', source, flags=re.S)
    def body(text, pattern):
        matches = list(re.finditer(pattern, text))
        if len(matches) != 1:
            raise ValueError("expected one dispatch body: " + pattern)
        start = matches[0].end()
        depth = 1
        for pos in range(start, len(text)):
            depth += (text[pos] == '{') - (text[pos] == '}')
            if not depth:
                return text[start:pos]
        raise ValueError("unterminated dispatch body: " + pattern)
    result = {}
    for name, (fn, selector) in SWITCHES.items():
        function = body(source, r'\b' + fn + r'\s*\([^;{}]*\)\s*\{')
        switch = body(function, r'\bswitch\s*\(\s*' + selector + r'\s*\)\s*\{')
        result[name] = {int(m.group(1), 16) for m in CASE.finditer(switch)}
        if not result[name]:
            raise ValueError("no opcode cases found in " + name)
    return result


def mpg_blocks(data):
    pos = 0
    while pos + 4 <= len(data):
        w = int.from_bytes(data[pos:pos + 4], "little")
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd == 0x4A:
            n = (num if num else 256) * 8
            if imm < 2048 and pos + 4 + n <= len(data):
                yield imm * 8, data[pos + 4:pos + 4 + n]
                pos += 4 + n
                continue
        pos += 4


def analyse(path, h):
    sec = ElfFile(path).section(".vutext")
    if not sec or not sec.data:
        return 0, collections.Counter(), collections.Counter()
    code = b"".join(b for _, b in mpg_blocks(sec.data))
    up_bad, lo_bad = collections.Counter(), collections.Counter()
    n = 0
    for off in range(0, len(code) - 7, 8):
        lower = int.from_bytes(code[off:off + 4], "little")
        upper = int.from_bytes(code[off + 4:off + 8], "little")
        n += 1
        funct = upper & 0x3F
        if (funct & 0x3C) == 0x3C:
            sub = (upper & 3) | ((upper >> 4) & 0x7C)
            if sub not in h["upper.sub"]:
                up_bad[("sub", sub)] += 1
        elif funct not in h["upper.funct"]:
            up_bad[("funct", funct)] += 1
        if upper & 0x80000000:
            continue
        op = (lower >> 25) & 0x7F
        if op == 0x40:
            if (lower & 0x3F) not in h["lower.low6"]                     and (lower & 0x7FF) not in h["lower.sub"]:
                lo_bad[("sub", lower & 0x7FF)] += 1
        elif op not in h["lower.op"]:
            lo_bad[("op", op)] += 1
    return n, up_bad, lo_bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", action="store_true",
                    help="also check the SDK samples' microcode")
    args = ap.parse_args()
    h = handled(VU_C)
    print("case labels parsed from %s:" % VU_C)
    for k, v in h.items():
        print("  %-14s %3d cases" % (k, len(v)))
    print()

    sec = ElfFile(GAME).section(".vutext")
    print(".vutext at %08X, %d bytes (a VIF packet stream, not raw microcode)"
          % (sec.addr, len(sec.data)))

    blocks = list(mpg_blocks(sec.data))
    if not blocks:
        raise ValueError("no VU microcode recovered")
    code = b"".join(b for _, b in blocks)
    print("MPG blocks: %d, carrying %d bytes = %d instructions (%.1f%% of the section)"
          % (len(blocks), len(code), len(code) // 8,
             100.0 * len(code) / len(sec.data)))
    print()

    up_bad, lo_bad = collections.Counter(), collections.Counter()
    n = n_imm = 0
    for off in range(0, len(code) - 7, 8):
        lower = int.from_bytes(code[off:off + 4], "little")
        upper = int.from_bytes(code[off + 4:off + 8], "little")
        n += 1
        funct = upper & 0x3F
        if (funct & 0x3C) == 0x3C:
            sub = (upper & 3) | ((upper >> 4) & 0x7C)
            if sub not in h["upper.sub"]:
                up_bad[("sub", sub)] += 1
        elif funct not in h["upper.funct"]:
            up_bad[("funct", funct)] += 1

        if upper & 0x80000000:
            n_imm += 1
            continue
        op = (lower >> 25) & 0x7F
        if op == 0x40:
            if (lower & 0x3F) not in h["lower.low6"] \
                    and (lower & 0x7FF) not in h["lower.sub"]:
                lo_bad[("sub", lower & 0x7FF)] += 1
        elif op not in h["lower.op"]:
            lo_bad[("op", op)] += 1

    print("decoded %d instructions (%d carried an I-bit immediate)" % (n, n_imm))
    print()
    for label, bad in (("UPPER", up_bad), ("LOWER", lo_bad)):
        total = sum(bad.values())
        print("=== %s: %d unhandled of %d (%.4f%%) ==="
              % (label, total, n, 100.0 * total / max(1, n)))
        for (kind, val), c in bad.most_common(25):
            print("    %-6s 0x%03X   %6d occurrences" % (kind, val, c))
        if not bad:
            print("    none")
        print()

    if args.corpus:
        print("=== SDK sample microcode (independent corpus) ===")
        tot = collections.Counter()
        files = up_tot = lo_tot = 0
        for f in sorted(glob.glob("sdk_corpus/**/*.elf", recursive=True)):
            cnt, ub, lb = analyse(f, h)
            if not cnt:
                continue
            files += 1
            tot["insns"] += cnt
            up_tot += sum(ub.values())
            lo_tot += sum(lb.values())
            if ub or lb:
                print("  %-46s %5d insns  UPPER %d  LOWER %d"
                      % (f[12:], cnt, sum(ub.values()), sum(lb.values())))
                for (k, v), c in list(ub.most_common(4)) + list(lb.most_common(4)):
                    print("        %-5s 0x%03X  x%d" % (k, v, c))
        print("  %d ELFs with microcode, %d instructions"
              % (files, tot["insns"]))
        print("  UPPER unhandled: %d      LOWER unhandled: %d" % (up_tot, lo_tot))
        if not files or up_tot or lo_tot:
            return 1
    return int(bool(up_bad or lo_bad))


if __name__ == "__main__":
    sys.exit(main())
