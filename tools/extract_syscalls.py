import struct, sys, os, json

def ar_members(data):
    assert data[:8] == b'!<arch>\n', 'not an ar archive'
    off, longnames, out = 8, b'', []
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        raw = hdr[0:16].decode('latin1')
        size = int(hdr[48:58].decode('latin1').strip() or 0)
        off += 60
        body = data[off:off + size]
        off += size + (size & 1)
        name = raw.rstrip()
        if name.startswith('//'):
            longnames = body
            continue
        if name.startswith('/') and name[1:].strip().isdigit():
            i = int(name[1:].strip())
            end = longnames.find(b'/', i)
            name = longnames[i:end].decode('latin1')
        name = name.rstrip('/')
        if name == '/':
            continue
        out.append((name, body))
    return out

def elf32_sections(d):
    assert d[:4] == b'\x7fELF'
    (e_shoff,) = struct.unpack_from('<I', d, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from('<10I', d, o)
        secs.append(dict(name_off=name, type=typ, addr=addr, offset=offset,
                         size=size, link=link, info=info, entsize=entsize))
    strtab = secs[e_shstrndx]
    sh = d[strtab['offset']:strtab['offset'] + strtab['size']]
    for s in secs:
        e = sh.find(b'\0', s['name_off'])
        s['name'] = sh[s['name_off']:e].decode('latin1')
    return secs

def elf32_symbols(d, secs):
    syms = []
    for s in secs:
        if s['type'] != 2:
            continue
        st = secs[s['link']]
        strs = d[st['offset']:st['offset'] + st['size']]
        n = s['size'] // 16
        for i in range(n):
            o = s['offset'] + i * 16
            nameoff, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, o)
            e = strs.find(b'\0', nameoff)
            name = strs[nameoff:e].decode('latin1')
            syms.append(dict(name=name, value=value, size=size,
                             bind=info >> 4, type=info & 0xF, shndx=shndx))
    return syms

def main(libpath, out_json):
    data = open(libpath, 'rb').read()
    table = {}
    for member, body in ar_members(data):
        if not body[:4] == b'\x7fELF':
            continue
        secs = elf32_sections(body)
        syms = elf32_symbols(body, secs)
        byidx = {i: s for i, s in enumerate(secs)}
        for sym in syms:
            if sym['type'] != 2 or sym['shndx'] >= len(secs):
                continue
            sec = byidx[sym['shndx']]
            if not sec['name'].startswith('.text'):
                continue
            start = sec['offset'] + sym['value']
            size = sym['size'] or 32
            code = body[start:start + size]
            words = [struct.unpack_from('<I', code, i)[0]
                     for i in range(0, len(code) - 3, 4)]
            for j, w in enumerate(words):
                if j + 1 < len(words) and words[j + 1] == 0x0000000C:
                    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
                    imm = w & 0xFFFF
                    if op == 9 and rs == 0 and rt == 3:
                        num = imm - 0x10000 if imm & 0x8000 else imm
                        table.setdefault(num, set()).add(sym['name'])
                    elif w == 0 or (op == 0 and (w & 0x3F) == 0x25 and rt == 0):
                        table.setdefault(0, set()).add(sym['name'])
    flat = {str(k): sorted(v) for k, v in sorted(table.items())}
    json.dump(flat, open(out_json, 'w'), indent=1)
    print(f'{len(flat)} syscall numbers extracted from {libpath}')
    for k in sorted(table, key=int):
        print(f'  {k:5d}  {", ".join(sorted(table[k]))}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
