from pathlib import Path
import random
r = random.Random(20260912)
SIZE = 4 * 1024 * 1024
MASK = 0xffffffff
cases = pixels = 0
for bits in (4, 8):
    offsets = list(range(0, 2048, bits))
    for trial in range(3000):
        r.shuffle(offsets)
        run = r.randint(1, 32 if bits == 4 else 16)
        row = offsets[:run]
        base = r.choice((0, 256, SIZE-256, SIZE, SIZE+256))
        block = r.randbytes(256)
        pal = [r.getrandbits(32) for _ in range(256)]
        seed = r.getrandbits(32)
        old, new = [], []
        oh = nh = seed
        for off in row:
            bit = base * 8 + off
            addr = (bit >> 3) & MASK
            raw = block[addr-base] if addr < SIZE else 0
            if bits == 4:
                raw = (raw >> (bit & 4)) & 15
            old.append(pal[raw])
            oh = ((oh ^ raw) * 16777619) & MASK
        for off in row:
            raw = block[off >> 3] if base <= SIZE-256 else 0
            if bits == 4:
                raw = (raw >> (off & 4)) & 15
            new.append(pal[raw])
            nh = ((nh ^ raw) * 16777619) & MASK
        assert (old, oh) == (new, nh)
        cases += 1
        pixels += run
for trial in range(10000):
    offsets = r.sample(range(256), 16)
    i = r.randrange(8)
    run = r.randint(1, min(8-i, 16))
    base = r.choice((0, SIZE-256, SIZE, SIZE+256))
    qw = r.getrandbits(64)
    old = bytearray(r.randbytes(256))
    new = old.copy()
    for k in range(run):
        if base+offsets[k] < SIZE:
            old[offsets[k]] = (qw >> ((i+k)*8)) & 255
    if base <= SIZE-256:
        packed = qw >> (i*8)
        for k in range(run):
            new[offsets[k]] = packed & 255
            packed >>= 8
    assert old == new
source = (Path(__file__).resolve().parents[1]/'runtime/src/ps2_gs.c').read_text()
assert 'const u32 raw = block[offsets[k] >> 3];' in source
assert 'u64 packed = qw >> (i * 8u);' in source
assert source.index('if (PS2_UNLIKELY(traced))') < source.index('u64 packed = qw >> (i * 8u);')
print(f'PASS: {cases} decode runs / {pixels} pixels and 10000 upload runs; identical output/hash/writes. Model checks only.')
