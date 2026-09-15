import argparse
import json
import os
import sys
import time

from .elf import ElfFile
from .analysis import Program
from .emit import Emitter


def main(argv=None):
    ap = argparse.ArgumentParser(prog="ps2recomp")
    ap.add_argument("elf", help="PS2 EE ELF executable")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--ida-db", help="JSON exported by tools/ida/export_db.py")
    ap.add_argument("--ida-seeds",
                    help="JSON exported by tools/ida/export_seeds.py")
    ap.add_argument("--per-file", type=int, default=250,
                    help="functions per generated .c file")
    ap.add_argument("--comments", action="store_true",
                    help="annotate each line with its guest disassembly")
    ap.add_argument("--symbols", action="append", default=[],
                    help="JSON {addr: {name:..}} or {addr: name}; repeatable")
    ap.add_argument("--overrides",
                    help="JSON {symbol-or-addr: native_handler}; the guest body "
                         "is replaced by a call to that handler")
    ap.add_argument("--hooks",
                    help="JSON {symbol-or-addr: native_handler}; the handler is "
                         "called on entry, before the recompiled body runs")
    ap.add_argument("--report", help="write a JSON analysis report here")
    args = ap.parse_args(argv)

    t0 = time.time()
    elf = ElfFile(args.elf)
    print("elf: entry=%08X  text=%s" % (
        elf.entry, ["%08X..%08X" % r for r in elf.text_ranges()]))

    prog = Program(elf, args.ida_db)
    prog.seed()

    if args.ida_seeds:
        with open(args.ida_seeds) as fp:
            seeds = json.load(fp)
        extra = 0
        for key in ("xref", "ptr"):
            for a in seeds.get(key, ()):
                if prog.in_text(a) and a not in prog.entries:
                    prog.entries.add(a)
                    extra += 1
        print("seeds: +%d indirect-call targets from data references" % extra)

    prog.partition()

    symbols = {}
    provenance = {}
    for path in args.symbols:
        with open(path) as fp:
            raw = json.load(fp)
        for k, v in raw.items():
            if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
                continue
            addr = int(k, 0) if isinstance(k, str) else int(k)
            name = v["name"] if isinstance(v, dict) else v
            symbols[addr] = name
            provenance[addr] = v if isinstance(v, dict) else {"name": v, "lib": "unspecified"}
    if symbols:
        print("symbols: %d names loaded" % len(symbols))

    overrides, missing = {}, []
    if args.overrides:
        with open(args.overrides) as fp:
            raw = json.load(fp)
        by_name = {}
        for a, n in symbols.items():
            if n in by_name and by_name[n] != a:
                by_name[n] = None
            else:
                by_name[n] = a
        for key, handler in raw.items():
            if key.startswith("//") or key.startswith("#"):
                continue
            if key.startswith("0x") or key.isdigit():
                addr = int(key, 0)
            else:
                addr = by_name.get(key)
                if addr is not None and "(aligned)" in provenance[addr].get("lib", ""):
                    raise ValueError("override requires verified address, not aligned SDK name: " + key)
                if addr is None:
                    missing.append(key)
                    continue
            if addr not in prog.functions:
                missing.append("%s@%08X (not a recovered function)"
                               % (key, addr))
                continue
            overrides[addr] = handler
        print("overrides: %d functions replaced by native handlers"
              % len(overrides))
        for m in missing:
            print("   override target not found: %s" % m)

    hooks = {}
    if args.hooks:
        with open(args.hooks) as fp:
            raw = json.load(fp)
        by_name = {}
        for a, n in symbols.items():
            if n in by_name and by_name[n] != a:
                by_name[n] = None
            else:
                by_name[n] = a
        for key, handler in raw.items():
            if key.startswith("//") or key.startswith("#"):
                continue
            addr = int(key, 0) if key.startswith("0x") else by_name.get(key)
            if addr is None or addr not in prog.functions:
                missing.append("hook: " + key)
                continue
            if not key.startswith("0x") and "(aligned)" in provenance.get(addr, {}).get("lib", ""):
                raise ValueError("hook requires verified address, not aligned SDK name: " + key)
            hooks[addr] = handler
        print("hooks: %d entry hooks" % len(hooks))

    if missing:
        if args.report:
            with open(args.report, "w") as fp:
                json.dump({"status": "failed", "missing_targets": missing,
                           "resolved_overrides": overrides, "resolved_hooks": hooks,
                           "symbol_provenance": provenance}, fp, indent=1)
        raise ValueError("unresolved override/hook targets: " + ", ".join(missing))
    os.makedirs(args.outdir, exist_ok=True)
    em = Emitter(prog, {"comments": args.comments,
                        "symbols": symbols,
                        "overrides": overrides,
                        "hooks": hooks})
    nfiles, nfuncs = em.emit_all(args.outdir, per_file=args.per_file)

    base, img = elf.image()
    with open(os.path.join(args.outdir, "ps2_image.bin"), "wb") as fp:
        fp.write(img)
    with open(os.path.join(args.outdir, "ps2_image.c"), "w") as fp:
        fp.write("/* generated by ps2recomp -- do not edit */\n")
        fp.write('#include "ps2_runtime.h"\n')
        fp.write("const u32 ps2_image_base = 0x%08Xu;\n" % base)
        fp.write("const u32 ps2_image_size = 0x%08Xu;\n" % len(img))

    if em.unhandled:
        print("UNHANDLED OPCODES: %s" % sorted(em.unhandled.items(),
                                               key=lambda kv: -kv[1]))
    if em.warnings:
        print("warnings: %d" % len(em.warnings))
        for wmsg in em.warnings[:20]:
            print("   " + wmsg)

    report = dict(prog.stats)
    report["generated_files"] = nfiles
    report["generated_functions"] = nfuncs
    report["unhandled"] = em.unhandled
    report["warnings"] = em.warnings[:200]
    report["symbol_provenance"] = {"0x%08X" % a: v for a, v in provenance.items()}
    report["applied_overrides"] = {"0x%08X" % a: h for a, h in em.overrides_used.items()}
    report["applied_hooks"] = {"0x%08X" % a: h for a, h in hooks.items()}
    report["missing_targets"] = missing
    report["delay_slot_entries"] = prog.pathological_delay_slots()
    report["uncovered_ranges"] = [[a, b] for a, b in prog.uncovered_ranges()]
    report["unresolved_indirect"] = [
        a for f in prog.functions.values() for a in f.indirect_jumps]
    report["image_base"] = base
    report["image_size"] = len(img)
    if args.report:
        with open(args.report, "w") as fp:
            json.dump(report, fp, indent=1)

    print("emitted %d functions into %d files in %.1fs"
          % (nfuncs, nfiles, time.time() - t0))
    return int(bool(em.unhandled))


if __name__ == "__main__":
    sys.exit(main())
