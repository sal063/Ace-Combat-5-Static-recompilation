from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_pcsx2_save as tool

STRIDE, PAGE, PPC, CLUSTERS = 528, 512, 2, 8192
CSIZE = PAGE * PPC
FAT_AT, ALLOC_OFF = 9, 41
rng = random.Random(1)


class Builder:
    def __init__(self):
        self.raw = bytearray(STRIDE * PPC * CLUSTERS)
        self.fat = [0x7FFFFFFF] * CLUSTERS
        self.free = list(range(1, CLUSTERS - ALLOC_OFF))
        rng.shuffle(self.free)

    def put(self, n, data):
        for i in range(PPC):
            p = (n * PPC + i) * STRIDE
            self.raw[p:p + PAGE] = data[i * PAGE:(i + 1) * PAGE].ljust(PAGE, b"\0")

    def store(self, data, first=None):
        chain = [first if first is not None else self.free.pop()]
        while len(chain) * CSIZE < len(data):
            chain.append(self.free.pop())
        for i, n in enumerate(chain):
            self.fat[n] = 0x80000000 | chain[i + 1] if i + 1 < len(chain) else 0xFFFFFFFF
            self.put(n + ALLOC_OFF, data[i * CSIZE:(i + 1) * CSIZE])
        return chain[0]

    @staticmethod
    def entry(name, mode, length, cluster, stamp):
        b = bytearray(512)
        struct.pack_into("<HHI8sI", b, 0, mode, 0, length, stamp, cluster)
        b[24:32] = stamp
        b[64:64 + len(name)] = name.encode()
        return bytes(b)

    def folder(self, name, files, stamp):
        rows = [self.entry(".", 0x8427, 0, 0, stamp), self.entry("..", 0x8427, 0, 0, stamp)]
        for fname, data in files.items():
            rows.append(self.entry(fname, 0x8497, len(data), self.store(data), stamp))
        return self.entry(name, 0x8427, len(rows), self.store(b"".join(rows)), stamp)

    def finish(self, folders):
        stamp = bytes(8)
        rows = [self.entry(".", 0x8427, len(folders) + 2, 0, stamp),
                self.entry("..", 0x8427, 0, 0, stamp)] + folders
        self.store(b"".join(rows), first=0)
        sb = bytearray(b"Sony PS2 Memory Card Format 1.2.0.0".ljust(PAGE, b"\0"))
        struct.pack_into("<HHHHIIII", sb, 40, PAGE, PPC, 16, 0, CLUSTERS, ALLOC_OFF,
                         CLUSTERS - 1, 0)
        struct.pack_into("<I", sb, 80, 8)
        self.put(0, bytes(sb))
        self.put(8, struct.pack("<32I", *range(FAT_AT, FAT_AT + 32)))
        for i in range(32):
            self.put(FAT_AT + i, struct.pack("<256I", *self.fat[i * 256:(i + 1) * 256]))
        return bytes(self.raw)


def run(*args, ok=True):
    r = subprocess.run([sys.executable, str(Path(tool.__file__)), *map(str, args)],
                       capture_output=True, text=True)
    if (r.returncode == 0) != ok:
        raise SystemExit("import_pcsx2_save %s: %s%s" % (args, r.stdout, r.stderr))
    return r.stdout + r.stderr


save = {"icon.sys": rng.randbytes(964), "icon1.ico": rng.randbytes(18272),
        "BASLUS-20851AC5": rng.randbytes(41104)}
stamp = bytes([0, 30, 26, 21, 16, 6]) + struct.pack("<H", 2026)
card = Builder()
folders = [card.folder("BASLUS-20851AC5", save, stamp),
           card.folder("BASLUS-20963", {"other": b"not ours"}, stamp)]

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "Mcd001.ps2").write_bytes(card.finish(folders))
    rt = tmp / "saves" / "card0.ps2mc"
    run(tmp / "Mcd001.ps2", rt)

    entries = {n: (b, d) for n, b, d in tool.load_runtime(str(rt))}
    assert sorted(entries) == ["/", "/BASLUS-20851AC5"] + sorted(
        "/BASLUS-20851AC5/" + f for f in save), sorted(entries)
    for f, data in save.items():
        block, got = entries["/BASLUS-20851AC5/" + f]
        assert got == data, f
        assert block[:16] == stamp * 2 and struct.unpack_from("<IH", block, 16) == (len(data), 0x8497)
    assert struct.unpack_from("<H", entries["/BASLUS-20851AC5"][0], 20)[0] == 0x8427

    assert "--replace" in run(tmp / "Mcd001.ps2", rt, ok=False)
    run(tmp / "Mcd001.ps2", rt, "--replace")
    assert len(tool.load_runtime(str(rt))) == len(entries)
    assert len(list(rt.parent.glob("card0.ps2mc.bak-*"))) == 1

    assert "not a formatted" in run(rt, tmp / "out.ps2mc", ok=False)

print("import_pcsx2_save: ok")
