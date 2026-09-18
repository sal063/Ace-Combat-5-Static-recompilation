import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def replay(capture, fields, shot, env_extra):
    exe = os.path.join(ROOT, "build", "gcc", "gsreplay.exe")
    env = dict(os.environ)
    env["PS2_SHADER_DIR"] = os.path.join(ROOT, "build", "gcc", "shaders")
    env["PS2_PRESENT_MODE"] = "immediate"
    env.update(env_extra)
    log = shot.replace(".ppm", ".log")
    with open(log, "w") as fp:
        r = subprocess.run([exe, capture, "--loop", "1", "--fields", str(fields),
                            "--shot", shot], cwd=ROOT, env=env, stdout=fp,
                           stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(shot):
        sys.exit("gsreplay failed for %s (see %s)" % (shot, log))
    return np.asarray(Image.open(shot).convert("RGB")).astype(np.int16), log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--fields", type=int, nargs="+", required=True)
    ap.add_argument("--switch", default="PS2_RN_SUN")
    ap.add_argument("--out", default=os.path.join("out", "rn", "compare"))
    ap.add_argument("--env", action="append", default=[],
                    help="extra NAME=VALUE for both runs")
    args = ap.parse_args()
    out = os.path.join(ROOT, args.out)
    os.makedirs(out, exist_ok=True)
    extra = dict(e.split("=", 1) for e in args.env)
    worst = 0
    for f in args.fields:
        a, la = replay(args.capture, f, os.path.join(out, "f%04d_emulated.ppm" % f),
                       dict(extra, **{args.switch: "0"}))
        b, lb = replay(args.capture, f, os.path.join(out, "f%04d_native.ppm" % f),
                       dict(extra, **{args.switch: "1"}))
        if a.shape != b.shape:
            print("field %d: picture sizes differ %s vs %s" % (f, a.shape, b.shape))
            continue
        d = np.abs(a - b)
        mask = d.max(axis=2) > 2
        n = int(mask.sum())
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(
            os.path.join(out, "f%04d_diff.png" % f))
        sheet = np.concatenate([a, b, np.clip(d * 4, 0, 255)], axis=1)
        Image.fromarray(sheet.astype(np.uint8)).save(
            os.path.join(out, "f%04d_sheet.png" % f))
        claimed = native = "?"
        for line in open(lb, errors="replace"):
            if "native sun --" in line:
                claimed = line.strip().split("rn: ", 1)[-1]
            if "native draws (rn.h claims)" in line:
                native = line.strip().split("vk: ", 1)[-1]
        if n:
            ys, xs = np.nonzero(mask)
            print("field %4d: %6d pixels differ in x %d..%d y %d..%d, mean %.1f "
                  "max %d" % (f, n, xs.min(), xs.max(), ys.min(), ys.max(),
                              float(d[mask].mean()), int(d.max())))
        else:
            print("field %4d: identical" % f)
        print("            %s" % claimed)
        print("            %s" % native)
        worst = max(worst, n)
    print("sheets (emulated | native | difference x4) in %s" % out)


if __name__ == "__main__":
    main()
