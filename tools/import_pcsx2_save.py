import argparse
import os
import shutil
import struct
import time

RT_MAGIC = b"AC5MC01\0"


class Ps2Card:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        if not self.raw.startswith(b"Sony PS2 Memory Card Format "):
            raise SystemExit("%s is not a formatted PS2 memory card" % path)
        (self.page_len, self.ppc, _, _, clusters, self.alloc_off, _,
         self.root) = struct.unpack_from("<HHHHIIII", self.raw, 40)
        self.ifc = struct.unpack_from("<32I", self.raw, 80)
        self.stride = len(self.raw) // (clusters * self.ppc)
        self.csize = self.page_len * self.ppc

    def cluster(self, n):
        return b"".join(self.raw[p * self.stride:p * self.stride + self.page_len]
                        for p in range(n * self.ppc, (n + 1) * self.ppc))

    def fat(self, n):
        per = self.csize // 4
        table = struct.unpack_from("<I", self.cluster(self.ifc[n // per // per]), n // per % per * 4)[0]
        return struct.unpack_from("<I", self.cluster(table), n % per * 4)[0]

    def read(self, n, size):
        out = b""
        for _ in range(size // self.csize + 1):
            out += self.cluster(n + self.alloc_off)
            nxt = self.fat(n)
            if len(out) >= size or nxt == 0xFFFFFFFF or not nxt & 0x80000000:
                break
            n = nxt & 0x7FFFFFFF
        if len(out) < size:
            raise SystemExit("a file on the PCSX2 card is cut short")
        return out[:size]

    def listdir(self, d):
        data = self.read(d["cluster"], d["length"] * 512)
        out = []
        for b in (data[i:i + 512] for i in range(0, len(data), 512)):
            mode, _, length = struct.unpack_from("<HHI", b)
            name = b[64:96].split(b"\0")[0].decode("latin-1")
            if mode & 0x8000 and name not in (".", ".."):
                out.append(dict(mode=mode, length=length, created=b[8:16], modified=b[24:32],
                                cluster=struct.unpack_from("<I", b, 16)[0], name=name))
        return out


def info(created, modified, mode):
    return bytearray(created + modified + struct.pack("<IH", 0, mode)).ljust(64, b"\0")


def load_runtime(path):
    t = time.localtime()
    now = bytes([0, t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon]) + struct.pack("<H", t.tm_year)
    if not os.path.exists(path):
        return [("/", info(now, now, 0x8027), b"")]
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] != RT_MAGIC:
        raise SystemExit("%s is not this runtime's memory card" % path)
    count = struct.unpack_from("<I", raw, 8)[0]
    pos, entries = 16, []
    for _ in range(count):
        name = raw[pos:pos + 256].split(b"\0")[0].decode("latin-1")
        block = bytearray(raw[pos + 256:pos + 320])
        size = struct.unpack_from("<I", block, 16)[0]
        entries.append((name, block, raw[pos + 320:pos + 320 + size]))
        pos += 320 + size
    if not any(e[0] == "/" for e in entries):
        entries.insert(0, ("/", info(now, now, 0x8027), b""))
    return entries


def save_runtime(path, entries):
    if len(entries) > 256 or sum(len(d) for _, _, d in entries) > 8 << 20 \
            or 16 + sum(1 + (len(d) + 1023) // 1024 for _, _, d in entries) > 8192:
        raise SystemExit("the saves don't fit on the runtime card")
    out = bytearray(RT_MAGIC + struct.pack("<II", len(entries), 1))
    for name, block, data in entries:
        struct.pack_into("<I", block, 16, len(data))
        out += name.encode("latin-1").ljust(256, b"\0") + block + data
    if os.path.exists(path):
        backup = path + time.strftime(".bak-%Y%m%d-%H%M%S")
        shutil.copy2(path, backup)
        print("backed up the old card to", backup)
    else:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path + ".tmp", "wb") as f:
        f.write(out)
    os.replace(path + ".tmp", path)


def game_running(path):
    try:
        with open(path + ".lock", "a"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcsx2_card")
    ap.add_argument("runtime_card")
    ap.add_argument("--replace", action="store_true")
    a = ap.parse_args()

    ps2 = Ps2Card(a.pcsx2_card)
    root = dict(cluster=ps2.root, length=struct.unpack_from("<I", ps2.read(ps2.root, 512), 4)[0])
    folders = [d for d in ps2.listdir(root) if d["mode"] & 0x20 and "SLUS-20851" in d["name"]]
    if not folders:
        raise SystemExit("no Ace Combat 5 save on %s" % a.pcsx2_card)
    if game_running(a.runtime_card):
        raise SystemExit("quit the game first; it has %s open" % a.runtime_card)

    entries = load_runtime(a.runtime_card)
    for d in folders:
        top = "/" + d["name"]
        inside = [e for e in entries if e[0] == top or e[0].startswith(top + "/")]
        if inside and not a.replace:
            raise SystemExit("%s is already on the runtime card; add --replace to overwrite it" % top)
        entries = [e for e in entries if e not in inside]
        entries.append((top, info(d["created"], d["modified"], d["mode"]), b""))
        for f in ps2.listdir(d):
            if not f["mode"] & 0x20:
                entries.append((top + "/" + f["name"], info(f["created"], f["modified"], f["mode"]),
                                ps2.read(f["cluster"], f["length"])))
        print("copied", top)
    save_runtime(a.runtime_card, entries)


if __name__ == "__main__":
    main()
