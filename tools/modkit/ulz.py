import struct

MAGIC = 0x1A7A6C55
HEADER = 16
DEFAULT_SHIFT = 11
DEFAULT_FLAGS = 0x02


class UlzError(Exception):
    pass


def is_ulz(buf, off=0):
    return len(buf) >= off + HEADER and \
        struct.unpack_from("<I", buf, off)[0] == MAGIC


def header(buf, off=0):
    if not is_ulz(buf, off):
        raise UlzError("no ULZ magic at %#x" % off)
    _, w1, w2, w3 = struct.unpack_from("<IIII", buf, off)
    return (w1 & 0xFFFFFF, (w2 >> 24) & 0xFF, w2 & 0xFFFFFF, w3 & 0xFFFFFF,
            (w1 >> 24) & 0xFF)


def decode(buf, off=0, limit=None):
    size, shift, lit, tok, _ = header(buf, off)
    want = size if limit is None else min(size, limit)
    mask = (1 << shift) - 1
    lit += off
    tok += off
    ctrl = off + HEADER
    out = bytearray()
    append = out.append
    try:
        while len(out) < want:
            (cw,) = struct.unpack_from("<I", buf, ctrl)
            ctrl += 4
            for _ in range(32):
                if len(out) >= want:
                    break
                if cw & 0x80000000:
                    append(buf[lit])
                    lit += 1
                else:
                    token = buf[tok] | (buf[tok + 1] << 8)
                    tok += 2
                    dist = (token & mask) + 1
                    if dist > len(out):
                        raise UlzError("back-reference before the start of output")
                    for _ in range(min((token >> shift) + 3, want - len(out))):
                        append(out[-dist])
                cw = (cw << 1) & 0xFFFFFFFF
    except (IndexError, struct.error):
        raise UlzError("stream ends before %d bytes were decoded" % want) from None
    return bytes(out)


def encode_stored(data, shift=DEFAULT_SHIFT, flags=DEFAULT_FLAGS):
    n = len(data)
    if n > 0xFFFFFF:
        raise UlzError("ULZ stores its size in 24 bits; %d is too large" % n)
    words = (n + 31) // 32
    ctrl = bytearray()
    remaining = n
    for _ in range(words):
        if remaining >= 32:
            ctrl += struct.pack("<I", 0xFFFFFFFF)
            remaining -= 32
        else:
            ctrl += struct.pack("<I", (0xFFFFFFFF << (32 - remaining)) & 0xFFFFFFFF
                                if remaining else 0)
            remaining = 0
    lit_off = HEADER + len(ctrl)
    tok_off = lit_off + n
    out = bytearray()
    out += struct.pack("<IIII", MAGIC,
                       (flags << 24) | n,
                       (shift << 24) | lit_off,
                       tok_off)
    out += ctrl
    out += data
    return bytes(out)


def roundtrip_ok(data):
    return decode(encode_stored(data)) == data
