import argparse
import os
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "modkit"

from . import ulz
from .pac import Archive, Names, open_disc


def _names(args):
    return Names(args.names) if args.names else None


def _ref_len(root, name):
    if name.startswith("../"):
        cands = [os.path.join(root, name[3:].replace("/", os.sep))]
    else:
        rel = name.replace("/", os.sep)
        cands = [os.path.join(root, "makepack", rel), os.path.join(root, rel)]
    for c in cands:
        if os.path.isfile(c):
            return os.path.getsize(c)
    return None


def cmd_list(args):
    ar = open_disc(args.disc)
    nm = _names(args)
    print("%d members" % ar.count)
    print("%-5s %10s %10s %6s  %s" % ("idx", "packed", "unpacked", "files", "first names"))
    limit = args.limit or ar.count
    for i in range(min(limit, ar.count)):
        n = ar.file_count(i)
        names = nm.names_for(i, expect=n) if nm else None
        shown = ", ".join(names[:3]) + (" ..." if names and len(names) > 3 else "") \
            if names else "(unnamed)"
        print("%-5d %10d %10d %6d  %s"
              % (i, ar.entries[i][1], ar.unpacked[i], n, shown))
    ar.close()


def cmd_extract(args):
    ar = open_disc(args.disc)
    nm = _names(args)
    todo = [args.member] if args.member is not None else range(ar.count)
    total = written = cut = absent = 0
    for i in todo:
        member = ar.member(i)
        files = Archive.split(member)
        names = nm.names_for(i, expect=len(files)) if nm else None
        for k, blob in enumerate(files):
            if blob is None:
                absent += 1
                continue
            name = names[k] if names else None
            if name and args.ref_tree:
                n = _ref_len(args.ref_tree, name)
                if n is not None and n <= len(blob):
                    if n != len(blob):
                        cut += 1
                    blob = blob[:n]
            rel = name if name and not name.startswith("../") else "#%d" % k
            dst = os.path.join(args.out, "BIN", "DATA.PAC", "%04d" % i,
                               rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fp:
                fp.write(blob)
            written += len(blob)
            total += 1
    ar.close()
    print("extracted %d files, %d bytes, into %s (%d absent entries skipped)%s"
          % (total, written, args.out, absent,
             ("  (%d cut to a reference length)" % cut) if args.ref_tree else ""))


def cmd_verify(args):
    ar = open_disc(args.disc)
    nm = _names(args)
    fails = []

    def check(ok, what):
        print("  %-62s %s" % (what, "ok" if ok else "FAILED"))
        if not ok:
            fails.append(what)

    print("table: %d members, packed %d..%d" % (
        ar.count, ar.entries[0][0], ar.entries[-1][0] + ar.entries[-1][1]))

    bad_magic = bad_size = bad_layout = with_absent = 0
    for i in range(ar.count):
        raw = ar.raw(i)
        if not ulz.is_ulz(raw):
            bad_magic += 1
            continue
        if ulz.header(raw)[0] != ar.unpacked[i]:
            bad_size += 1
        offs = ar.header(i)
        ext = Archive.extents(offs, ar.unpacked[i])
        if 0 in offs:
            with_absent += 1
        sizes = [e if o else None for o, e in zip(offs, ext)]
        if Archive.layout(sizes) != (offs, ar.unpacked[i]):
            bad_layout += 1
    check(bad_magic == 0, "all %d members carry the ULZ magic" % ar.count)
    check(bad_size == 0, "every header's size matches the size table")
    check(bad_layout == 0, "rebuild layout matches all %d members (%d with absent files)"
          % (ar.count, with_absent))

    n = ar.count if args.members == 0 else min(args.members, ar.count)
    rebuilt = roundtrip = 0
    for i in range(n):
        member = ar.member(i)
        if Archive.build(Archive.split(member)) == member:
            rebuilt += 1
        if ulz.decode(ulz.encode_stored(member)) == member:
            roundtrip += 1
    check(rebuilt == n, "split/build reproduces %d members byte for byte" % n)
    check(roundtrip == n, "store-only encode round-trips %d members" % n)

    if nm:
        named = 0
        for i in range(ar.count):
            if nm.names_for(i, expect=ar.file_count(i)) is not None:
                named += 1
        pct = 100.0 * named / ar.count
        print("  %-62s %d/%d (%.1f%%)" % ("members whose names line up", named,
                                          ar.count, pct))
        check(pct > 95.0, "name mapping covers more than 95% of members")
    else:
        print("  (no --names given, so the name mapping was not checked)")

    ar.close()
    print("\n%s" % ("all checks passed" if not fails
                    else "FAILED: " + "; ".join(fails)))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="modkit")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("list", cmd_list), ("extract", cmd_extract),
                     ("verify", cmd_verify)):
        p = sub.add_parser(name)
        p.add_argument("--disc", required=True)
        p.add_argument("--names", help="datapack.bin from the PS4 release")
        if name == "list":
            p.add_argument("--limit", type=int, default=20)
        if name == "extract":
            p.add_argument("--out", required=True)
            p.add_argument("--member", type=int)
            p.add_argument("--ref-tree", dest="ref_tree",
                           help="a reference tree (the PS4 release's uroot) to "
                                "cut each file to its true length")
        if name == "verify":
            p.add_argument("--members", type=int, default=8,
                           help="how many members to decode and rebuild byte "
                                "for byte (0 = all; slow in Python -- "
                                "tools/test_vfs.py does all 965 in C)")
        p.set_defaults(fn=fn)
    args = ap.parse_args(argv)
    return args.fn(args) or 0


if __name__ == "__main__":
    sys.exit(main())
