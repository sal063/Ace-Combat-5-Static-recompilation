import random
from collections import deque

def check(seed):
    rng = random.Random(seed)
    cap = 1024
    ring = [None] * cap
    head = tail = cached = 0
    reserved = None
    snapshot = None
    expected = deque()
    pending = 0
    submitted = consumed = wraps = full = 0
    for step in range(40000):
        op = rng.randrange(4)
        if op == 0 and reserved is None and submitted < 5000:
            field = rng.randrange(17) == 0
            if field and pending == 2:
                continue
            n = 1 if field else rng.choice([2, 6, 9, 13, 22, 54, 262])
            off = head % cap
            pad = cap - off if off + n > cap else 0
            need = pad + n
            if cap - (head - cached) < need:
                cached = tail
            if cap - (head - cached) < need:
                full += 1
                continue
            assert head + need - tail <= cap
            if pad:
                ring[off] = ('wrap', pad)
                wraps += 1
                off = 0
            value = (submitted, n, field)
            ring[off] = value
            reserved = (head + need, value)
            if field:
                pending += 1
        elif op == 1 and reserved:
            head, value = reserved
            expected.append(value)
            submitted += 1
            reserved = None
        elif op == 2 and snapshot is None and tail != head:
            snapshot = [tail, head, 0]
        elif op == 3 and snapshot:
            local, end, count = snapshot
            value = ring[local % cap]
            if value[0] == 'wrap':
                local += value[1]
                field = False
            else:
                assert value == expected.popleft(), (seed, step, value)
                _, n, field = value
                local += n
                count += 1
                consumed += 1
            if local == end or count == 256 or field:
                tail = local
                if field:
                    pending -= 1
                snapshot = None
            else:
                snapshot = [local, end, count]
        assert 0 <= head - tail <= cap
        assert 0 <= pending <= 2
    assert wraps > 50 and full > 0 and consumed > 1000
    return consumed

if __name__ == '__main__':
    total = sum(check(seed) for seed in range(100))
    print(f'PASS: {total} records, 100 interleaving seeds; wrap, full ring, field boundaries, unpublished ownership')
