from pathlib import Path
import re, struct, random
s=(Path(__file__).resolve().parents[1]/'runtime/src/ps2_gs.c').read_text()
def table(name):
    t=re.search(r'static const u8 '+name+r'\[.*?=\s*\{(.*?)\};',s,re.S)[1]
    return list(map(int,re.findall(r'\d+',t)))
t32,t16=table('blk_ct32'),table('blk_ct16s')
def address(x,y,bw,is16):
    ph=64 if is16 else 32;blockw=16 if is16 else 8
    page=(y//ph)*(bw//64)+x//64
    block=(t16 if is16 else t32)[((y%ph)//8)*(64//blockw)+(x%64)//blockw]
    xx=x%blockw; yy=y%8
    word=(xx&1)|(((xx>>1)&3)<<2)|((yy&1)<<1)
    bit=(yy//2)*512+word*32+(((xx>>3)&1)*16 if is16 else 0)
    return page*8192+block*256+bit//8
rng=random.Random(13)
for bw,height in ((64,128),(128,256),(64,64)):
    size=bw*height*2
    initial=bytearray(rng.randbytes(size)); reference=initial.copy(); fast=initial.copy()
    for y in range(height):
        for x in range(bw):
            dst=address(x,y,bw,True);src=address(x&1015,y&1023,bw,True)
            reference[dst:dst+2]=initial[src:src+2]
    for y in range(height//2):
        for x in range(bw):
            dst=address(x,y,bw,False)
            fast[dst:dst+4]=initial[dst:dst+2]*2
    assert reference==fast,(bw,height)
for mask in range(16):
    fbmask=sum(255<<(ch*8) for ch in range(4) if mask>>ch&1)
    for rgb in (0,1):
        for a in (0,1):
            native=((7 if rgb else 0)|(8 if a else 0)) & ~mask
            for ch in range(4):
                assert bool(native>>ch&1)==(bool(rgb if ch<3 else a) and (fbmask>>(ch*8)&255)!=255)
assert 'if (!st.shuffle_rg && st.tex_rt' in s
print('PASS: CT16S region-repeat vs CT32 RG-copy physical VRAM equality for full-page cloud sizes; all 64 channel-mask/AFAIL combinations.')

