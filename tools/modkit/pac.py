import bisect
import os
import struct

from . import ulz

TAIL_START = 814
TAIL_SHIFT = 66
ALIGN = 16


class Archive:

    def __init__(self, tbl_path, pac_path):
        tbl = open(tbl_path, "rb").read()
        (self.count,) = struct.unpack_from("<I", tbl, 0)
        want = 8 + 12 * self.count
        if want != len(tbl):
            raise ValueError("%s is %d bytes, expected %d for %d members"
                             % (tbl_path, len(tbl), want, self.count))
        self.entries = [struct.unpack_from("<II", tbl, 8 + 8 * i)
                        for i in range(self.count)]
        base = 8 + 8 * self.count
        self.unpacked = [struct.unpack_from("<I", tbl, base + 4 * i)[0]
                         for i in range(self.count)]
        self.pac_path = pac_path
        self._fp = None

    def _pac(self):
        if self._fp is None:
            self._fp = open(self.pac_path, "rb")
        return self._fp

    def close(self):
        if self._fp:
            self._fp.close()
            self._fp = None

    def raw(self, index):
        off, size = self.entries[index]
        fp = self._pac()
        fp.seek(off)
        return fp.read(size)

    def member(self, index):
        data = ulz.decode(self.raw(index))
        if len(data) != self.unpacked[index]:
            raise ValueError("member %d decoded to %d bytes, table says %d"
                             % (index, len(data), self.unpacked[index]))
        return data

    def file_count(self, index):
        head = ulz.decode(self.raw(index), limit=4)
        return struct.unpack_from("<I", head, 0)[0]

    def header(self, index):
        raw = self.raw(index)
        (n,) = struct.unpack_from("<I", ulz.decode(raw, limit=4), 0)
        return Archive.offsets(ulz.decode(raw, limit=4 + 4 * n))

    @staticmethod
    def offsets(member):
        (n,) = struct.unpack_from("<I", member, 0)
        return [struct.unpack_from("<I", member, 4 + 4 * i)[0] for i in range(n)]

    @staticmethod
    def extents(offs, total):
        present = sorted(o for o in offs if o)
        out = []
        for o in offs:
            if not o:
                out.append(0)
                continue
            i = bisect.bisect_right(present, o)
            out.append((present[i] if i < len(present) else total) - o)
        return out

    @staticmethod
    def split(member):
        offs = Archive.offsets(member)
        return [member[o:o + e] if o else None
                for o, e in zip(offs, Archive.extents(offs, len(member)))]

    @staticmethod
    def layout(sizes):
        off = _align(4 + 4 * len(sizes))
        offs = []
        for s in sizes:
            if s is None:
                offs.append(0)
                continue
            offs.append(off)
            off = _align(off + s)
        return offs, off

    @staticmethod
    def build(files):
        offs, total = Archive.layout([None if f is None else len(f) for f in files])
        out = bytearray(total)
        struct.pack_into("<I", out, 0, len(files))
        for i, (o, f) in enumerate(zip(offs, files)):
            struct.pack_into("<I", out, 4 + 4 * i, o)
            if f is not None:
                out[o:o + len(f)] = f
        return bytes(out)


def _align(n, to=ALIGN):
    return (n + to - 1) // to * to


class Names:

    def __init__(self, path):
        dp = open(path, "rb").read()
        self.word0, self.group_count = struct.unpack_from("<II", dp, 0)
        self.groups = [struct.unpack_from("<HH", dp, 8 + 4 * i)
                       for i in range(self.group_count)]
        self._rec0 = 8 + 4 * self.group_count
        self.record_count = (len(dp) - self._rec0) // 36
        self._dp = dp

    def record(self, n):
        off = self._rec0 + 36 * n
        (align,) = struct.unpack_from("<I", self._dp, off)
        name = self._dp[off + 4:off + 36].split(b"\x00")[0].decode("ascii", "replace")
        return align, name

    def group_of(self, member_index):
        g = member_index if member_index < TAIL_START else member_index + TAIL_SHIFT
        return g if 0 <= g < self.group_count else None

    def names_for(self, member_index, expect=None):
        g = self.group_of(member_index)
        if g is None:
            return None
        first, count = self.groups[g]
        if expect is not None and count != expect:
            return None
        return [self.record(first + k)[1] for k in range(count)]


def open_disc(root):
    tbl = os.path.join(root, "BIN", "DATA.TBL")
    pac = os.path.join(root, "BIN", "DATA.PAC")
    if not os.path.exists(tbl):
        tbl, pac = os.path.join(root, "DATA.TBL"), os.path.join(root, "DATA.PAC")
    return Archive(tbl, pac)
