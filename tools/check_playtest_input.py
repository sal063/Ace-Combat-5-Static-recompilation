from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
s=(root/'runtime/src/ps2_hle_pad.c').read_text()
body=s[s.index('void ps2_dbc_report('):]
expr={int(i):e.strip() for i,e in re.findall(r'out\[(\d)\]\s*=\s*([^;]+);',body)}
def axis(neg,pos):return max(0,min(255,128+int((pos-neg)/2)))
def decode(v):return min(255,2*(128-v)) if v<128 else min(255,2*(v-128))
expected={'l1':6,'l2':7,'r1':17,'r2':18,'sqr':14,'tri':15}
for pressed,want in expected.items():
 values=dict.fromkeys(expected,0);values[pressed]=255
 out={j:eval(expr[j],{'__builtins__':{},'dbc_axis':axis},values) for j in (2,3,6,7)}
 abstract={7 if out[2]<128 else 18:decode(out[2]),6 if out[3]<128 else 17:decode(out[3]),15:out[6],14:out[7]}
 held={i for i,p in abstract.items() if p>=32}
 assert held=={want},(pressed,held,want)
 print(pressed,'->',want,'PASS')
iso=(root/'runtime/src/ps2_iso.c').read_text()
assert '_fseeki64(fp, (s64)offset, SEEK_SET)' in iso
assert not re.search(r'\bfseek\(',iso)
assert 'disc_seek(iso_fp, (u64)lsn * ISO_SECTOR)' in iso
print('ISO offsets stay 64-bit at seek call sites: PASS')
assert 'cached = PAD_PATH_LIBPAD2;' in s
assert 'if (buf) ps2_w8(buf, 3);' in s
assert 'd[16] = (buttons >> PAD_L2) & 1 ? 255 : st->l2;' in s
assert 'd[17] = (buttons >> PAD_R2) & 1 ? 255 : st->r2;' in s
report=s[s.index('    d[14] ='):s.index('    if (buf) ps2_put_mem')]
assign=dict((int(i),e.strip()) for i,e in re.findall(r'd\[(\d+)\]\s*=\s*([^;]+);',report))
physical={'PAD_L1':10,'PAD_R1':11,'PAD_L2':8,'PAD_R2':9}
for combination in range(16):
    buttons=sum(1<<bit for j,bit in enumerate(physical.values()) if combination & (1<<j))
    values={**physical,'buttons':buttons,'l2':0,'r2':0}
    decoded=[]
    for i in (14,15,16,17):
        cond, arms=assign[i].split('?')
        yes,no=arms.split(':')
        no=no.strip().replace('st->','')
        pressure=eval(yes if eval(cond,{},values) else no,{},values)
        decoded.append(pressure)
    assert decoded==[255 if combination & (1<<j) else 0 for j in range(4)]
print('Standard pad capabilities, map selection, and 16 simultaneous shoulder combinations: PASS')
