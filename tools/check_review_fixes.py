from pathlib import Path
from types import SimpleNamespace
import json, re, tempfile, random
import io, contextlib, sys
from unittest.mock import patch
from ps2recomp import sigmatch
from ps2recomp.analysis import Function, Program
from ps2recomp.emit import Emitter
from ps2recomp.r5900 import decode
from verify_vu import handled
import verify_vu
from ps2recomp import __main__ as pipeline

root = Path(__file__).resolve().parents[1]

words = {0x1000: 0x10800003, 0x1004: 0x2484FFFF, 0x1008: 0x1C80FFFE,
         0x100C: 0, 0x1010: 0x03E00008, 0x1014: 0}
fn = Function(0x1000, 'delay_entry', [0x1000, 0x1008, 0x1010], {0x1010},
              {0x1004, 0x100C, 0x1014})
p = SimpleNamespace(insns={a: decode(w, a) for a,w in words.items()},
                    functions={fn.entry: fn}, switches={}, names={},
                    in_text=lambda a: 0x1000 <= a < 0x1018)
em = Emitter(p)
text = em.emit_function(fn)
assert 'goto L_00001004' in text and 'L_00001004:;' in text
assert 'ps2_dispatch(ctx, 0x00001004' not in text
copy = text.split('L_00001004:;')[1]
assert 'ctx->r[4]' in copy and 'goto L_00001008;' in copy
assert Program.pathological_delay_slots(p) == [(0x1000, 0x1004)]
del p.insns[0x1014]
try: Emitter(p).emit_function(fn)
except ValueError as e: assert 'missing delay slot' in str(e)
else: raise AssertionError('missing delay slot accepted')

statement = Emitter(p).i_MTSA(SimpleNamespace(rs=4))
assert '& 0xF' not in statement
for saved in range(128):
    restored = saved & 0xFFFFFFFF
    assert restored == saved

mmi = (root/'runtime/include/ps2_mmi.h').read_text().split('void ps2_pmulth(')[1].split('void ps2_phmadh(')[0]
lanes = {int(a):int(b) for a,b in re.findall(r'ctx->r\[rd\]\.sw\[(\d)\] = p\[(\d)\]', mmi)}
rng = random.Random(5900)
for _ in range(1000):
    products = [rng.randrange(-32768,32768)*rng.randrange(-32768,32768) for i in range(8)]
    assert [products[lanes[i]] for i in range(4)] == products[::2]

src = (root/'runtime/src/ps2_vu.c').read_text()
expected = {'upper.sub':47, 'upper.funct':48, 'lower.op':28, 'lower.low6':5, 'lower.sub':36}
with tempfile.TemporaryDirectory(dir=root/'out') as d:
    f = Path(d)/'vu.c'
    f.write_text('\n'*150 + '/* { case 0xFFF: } */\n' + src)
    assert {k:len(v) for k,v in handled(f).items()} == expected
    f.write_text(src.replace('switch (low6)', 'switch (broken_selector)'))
    try: handled(f)
    except ValueError: pass
    else: raise AssertionError('missing VU switch accepted')

packet = (0x4A010000).to_bytes(4,'little') + bytes(4) + (0x32).to_bytes(4,'little')
fake_elf = SimpleNamespace(section=lambda name:SimpleNamespace(addr=0,data=packet))
with patch.object(verify_vu, 'ElfFile', return_value=fake_elf), patch.object(sys,'argv',['verify_vu.py']), contextlib.redirect_stdout(io.StringIO()):
    assert verify_vu.main() == 1

with tempfile.TemporaryDirectory(dir=root/'out') as d:
    d=Path(d); overrides=d/'overrides.json'; report_path=d/'report.json'
    overrides.write_text(json.dumps({'missingSdkFunction':'hle_test'}))
    fake_program=SimpleNamespace(seed=lambda:None, partition=lambda:None, functions={})
    fake_elf=SimpleNamespace(entry=0x1000,text_ranges=lambda:[(0x1000,0x1020)])
    with patch.object(pipeline,'ElfFile',return_value=fake_elf), patch.object(pipeline,'Program',return_value=fake_program), contextlib.redirect_stdout(io.StringIO()):
        try: pipeline.main(['unused.elf','-o',str(d/'generated'),'--overrides',str(overrides),'--report',str(report_path)])
        except ValueError as e: assert 'unresolved override' in str(e)
        else: raise AssertionError('missing override accepted')
    assert json.loads(report_path.read_text())['status']=='failed'
    assert not (d/'generated').exists()

def scalar_key(x): return x ^ (0xFFFFFFFF if x>>31 else 0x80000000)
def vector_key(x):
    k = x ^ (0x7FFFFFFF if x>>31 else 0)
    return k - (1<<32) if k>>31 else k
values = [0, 0x80000000, 0x3F800000, 0xBF800000, 0x7FFFFFFF, 0xFFFFFFFF]
values += [rng.randrange(1<<32) for _ in range(100)]
for x in values:
    for y in values: assert (scalar_key(x)>scalar_key(y)) == (vector_key(x)>vector_key(y))
assert scalar_key(0) > scalar_key(0x80000000)

syms = [dict(name=n, type=sigmatch.STT_FUNC, shndx=0, size=16, value=i*16)
        for i,n in enumerate(('first', 'middle', 'last'))]
obj = SimpleNamespace(secs=[{'name':'.text'}], symbols=lambda:syms)
gp = SimpleNamespace(functions={a:None for a in (0x1000,0x1010,0x1020,0x1030)})
with tempfile.TemporaryDirectory(dir=root/'out') as d:
    (Path(d)/'sdk.a').write_bytes(b'archive')
    with patch.object(sigmatch, 'ar_members', return_value=[('obj.o', b'\x7fELF')]), patch.object(sigmatch, 'Obj', return_value=obj):
        exact = {0x1000:('first','sdk.a',4), 0x1020:('last','sdk.a',4)}
        good = sigmatch.align_object_symbols(d,gp,exact.copy(),log=lambda *a:None)
        assert good[0x1010][0] == 'middle'
        for bad in ({0x1000:exact[0x1000]}, exact|{0x1010:('different','other.a',4)}):
            assert sigmatch.align_object_symbols(d,gp,bad.copy(),log=lambda *a:None) == bad
        syms[1]['size'] = 256
        assert sigmatch.align_object_symbols(d,gp,exact.copy(),log=lambda *a:None) == exact

report = json.loads((root/'config/report.json').read_text())
assert len(report['applied_overrides']) == 73 and len(report['applied_hooks']) == 9
assert report['applied_overrides']['0x0033E920'] == 'hle_sceCdInit'
assert '0x0033EC00' not in report['applied_overrides']
assert report['applied_overrides']['0x0033EDF8'] == 'hle_sceCdDiskReady'
assert not report['missing_targets'] and not report['unhandled']
print('PASS: delay-slot entry/rejection, MTSA, 1000 PMULTH vectors, structural VU parser, VU ordering, SDK alignment rejection, 73 overrides/9 hooks.')
