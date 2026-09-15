import math
from fractions import Fraction
from pathlib import Path

def mode(p0,p1,t0,t1):
    dp=p1-p0;dt=t1-t0;span=abs(dp)
    if not span or (p0|p1|t0|t1)&7:return 0
    if span//math.gcd(span,abs(dt))>=32:return 0
    return 1 if dt<=0 or span&(span-1)==0 else 2

def sample(t,m,origin,p):
    if not m:return math.floor(t+1/1024)
    h=math.floor(t*2+0.5)/2
    if abs(t-h)<=1/64:t=h+(1 if m==1 or p==origin else -1)/64
    return math.floor(t)
assert mode(0,48,0,48)==2
assert sample(0,2,0,0)==0
assert sample(1,2,0,1)==0
assert mode(0,64,0,64)==1 and sample(1,1,0,1)==1
assert mode(0,48,48,0)==1
assert mode(0,48,0,47)==0
assert mode(0,264,0,8)==0
assert mode(4528,5424,896,1808)==0
assert sample(56+55*57/56,0,4528//16,338)==111
cases=0
for span in range(8,513,8):
 for dt in range(-256,257,8):
  m=mode(0,span,256,256+dt)
  if not m:continue
  for pixel in range((span+15)//16):
   t=Fraction(256,16)+Fraction(pixel*dt,span)
   if t.denominator==1:
    expected=int(t)-(m==2 and pixel!=0)
    assert sample(float(t),m,0,pixel)==expected
    cases+=1
for alpha in range(256):
    stored=min(255,math.floor(alpha/128*255+0.5))/255
    assert (stored>=1)==(alpha>=128)
for ta0 in (0,128):
 for ta1 in (0,128):
  for aem in (False,True):
   for half in range(65536):
    r=half&255;g=half>>8
    a=ta1 if half&32768 else ta0
    if aem and half&32767==0:a=0
    packed=(half&32767)|(a<<8)
    ta=ta1 if g&128 else ta0
    if aem and r==0 and g&127==0:ta=0
    out=r|(((g&127)|ta)<<8)
    assert packed==out
tex=[0,10,20,30,40,50,60,70]
for x in range(8):
    tc=x+0.5
    tap=tc-0.5
    assert math.floor(tap)==x and tap%1==0
assert (3&2)==2 and (4&2)==0
correct=0.5*tex[3&2]+0.5*tex[4&2]
wrong=0.5*tex[3&2]+0.5*tex[(3&2)+1]
assert correct==10 and wrong==25
for t in range(-100,100):assert ((t%8)+8)%8==t%8
s=Path('runtime/shaders/gs.frag').read_text()
assert 'flat in uint v_round_uv' in s
assert 'sample_region_repeat(unwrapped_tc)' in s
assert 'ad >= 1.0' in s
u = 0.3 - 0.5
assert (math.floor(u % 128), math.floor(u % 128) + 1) == (127, 128)
assert (math.floor(u) % 128, (math.floor(u) + 1) % 128) == (127, 0)
assert '(own && ((pc.flags.z & 3u) == 0u' in s

def wrap_native(t, mode, lo, hi, size):
    if mode == 0: return t % max(size, 1)
    if mode == 1: return min(max(t, 0), max(size - 1, 0))
    if mode == 2: return min(max(t, lo), max(hi, lo))
    return (t & lo) | hi
def wrap_up(k, mode, lo, hi, size, s):
    if mode == 0:
        period = max(size, 1) * s
        return k - period * math.floor(k / period)
    if mode == 1: return min(max(k, 0), max(size, 1) * s - 1)
    if mode == 2: return min(max(k, lo * s), max(hi, lo) * s + s - 1)
    g = math.floor(k / s)
    return ((g & lo) | hi) * s + (k - g * s)
modes = ((0, 0, 0, 128), (1, 0, 0, 128), (2, 17, 90, 128), (3, 1015, 0, 1024), (3, 1023, 0, 64))
upcases = 0
for mode, lo, hi, size in modes:
    for k in range(-300, 1300):
        assert wrap_up(k, mode, lo, hi, size, 1) == wrap_native(k, mode, lo, hi, size)
        upcases += 1
    for sc in (2, 3, 4, 8):
        for k in range(-300 * sc, 1300 * sc):
            g, w = math.floor(k / sc), wrap_up(k, mode, lo, hi, size, sc)
            if mode in (0, 3):
                assert w // sc == wrap_native(g, mode, lo, hi, size) and w % sc == k % sc
            else:
                wl, wh = (0, size * sc - 1) if mode == 1 else (lo * sc, hi * sc + sc - 1)
                assert wl <= w <= wh and (w == k or not wl <= k <= wh)
            upcases += 1
for sc in (1, 2, 3, 4, 8):
    for j in range(0, 64 * sc):
        u = (j + 0.5) / sc
        tc = u - 0.5
        c = (tc + 0.5) * sc - 0.5
        assert abs(c - j) < 1e-9
        n = j // sc
        g = math.floor((n + 0.5) + 1 / 32)
        assert g * sc + (j - n * sc) == j
        upcases += 1
U = lambda x: 56 + (x - 283) * 57 / 56
assert math.floor(U(338) + 1 / 1024) == 111
assert any(math.floor(U(338 + (m + 0.5) / 4 - 0.5) + 1 / 1024) == 112 for m in range(4))
assert 'sample_region_repeat_up(unwrapped_tc)' in s and 'RUV_SPRITE' in s
assert 'if (point && (sprite || rt_up))' in s
def row13_weight(v, half):
    t = v - 0.5 if half else v
    lo = math.floor(t)
    return t - lo if lo == 12 else (1.0 if lo >= 13 else 0.0)
assert abs(row13_weight(12.9, False) - 0.9) < 1e-9
assert abs(row13_weight(12.9, True) - 0.4) < 1e-9
assert row13_weight(12.5, True) == 0.0 and row13_weight(12.5, False) == 0.5
assert 'if (!point && (f & (1u << 25)) != 0u) tc -= 0.5;' in s
assert 'PS2_OLD_TEXEL_CENTRE' in Path('runtime/src/ps2_video.c').read_text()
print(f'PASS: {cases} exact-boundary sprite samples, 256 DATE values, 524288 TEXA/AEM halfword cases, bilinear centers, region-repeat and render-target repeat seams, {upcases} upscaled wrap/copy cases, filtered texel centres.')
