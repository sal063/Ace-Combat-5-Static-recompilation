import argparse
import bisect
import os
import shutil
import subprocess
import sys


def symbols(exe):
    nm = shutil.which("nm") or r"C:\msys64\ucrt64\bin\nm.exe"
    out = subprocess.run([nm, "-n", "-C", exe], capture_output=True, text=True).stdout
    addrs, names, base = [], [], None
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3 or parts[1] not in "tTwW":
            continue
        a = int(parts[0], 16)
        addrs.append(a)
        names.append(parts[2])
        if parts[2] in ("__image_base__", "__ImageBase"):
            base = a
    if base is None:
        for line in out.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[2] in ("__image_base__", "__ImageBase"):
                base = int(parts[0], 16)
    return addrs, names, base if base is not None else 0x140000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("profile")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    addrs, names, base = symbols(args.exe)
    size = os.path.getsize(args.exe)
    by_fn, outside, total, head = {}, 0, 0, []
    for line in open(args.profile):
        parts = line.split()
        if len(parts) != 2:
            continue
        if not parts[0].lstrip("-").isdigit():
            head.append(line.strip())
            continue
        rva, n = int(parts[0]), int(parts[1])
        total += n
        a = base + rva
        i = bisect.bisect_right(addrs, a) - 1
        if rva < 0 or i < 0 or rva > size * 4:
            outside += n
            continue
        by_fn[names[i]] = by_fn.get(names[i], 0) + n
    print(" ".join(head))
    if not total:
        return
    print("%6.2f%%  (outside the executable: waiting, drivers)" % (100.0 * outside / total))
    for name, n in sorted(by_fn.items(), key=lambda kv: -kv[1])[:args.top]:
        print("%6.2f%%  %s" % (100.0 * n / total, name))


if __name__ == "__main__":
    sys.exit(main())
