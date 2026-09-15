from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace
from ps2recomp.r5900 import decode
from ps2recomp.emit import Emitter

root = Path(__file__).resolve().parents[1]
out = root / 'out' / 'runtime_regressions'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'generated/ps2_code_0006.c').read_text()
start = source.index('void func_001A3B38(')
end = source.index('\n}\n', start) + 3
body = source[start:end]
harness = r'''
#include "ps2_runtime.h"
#include <stdio.h>
void ps2_preempt(void) {}
u32 ps2_loop_ctr;
void ps2_loop_service(void) {}
const ps2_func_entry ps2_func_table[1] = {{0}};
const unsigned ps2_func_count = 0;
const ps2_symbol ps2_symbols[1] = {{0}};
const unsigned ps2_symbol_count = 0;
u8 ps2_mmio_r8(u32 a) { abort(); }
void ps2_mmio_w8(u32 a, u8 v) { abort(); }
void ps2_mmio_r128(ps2_reg128 *v, u32 a) { abort(); }
u32 ps2_mmio_r32(u32 a) { abort(); }
void ps2_mmio_w128(u32 a, const ps2_reg128 *v) { abort(); }
void ps2_mmio_w32(u32 a, u32 v) { abort(); }
u64 ps2_mmio_r64(u32 a) { abort(); }
void ps2_mmio_w64(u32 a, u64 v) { abort(); }
'''
harness += body
cleanup_source = (root / 'generated/ps2_code_0035.c').read_text()
start = cleanup_source.index('void func_00356C38(')
end = cleanup_source.index('\n}\n', start) + 3
harness += cleanup_source[start:end]
harness += cleanup_source[start:end].replace('func_00356C38', 'old_cleanup_entry').replace('    goto L_00356C38;\n', '')
words = [0x50800001, 0, 0, 0, 0x48428800, 0x03E00008, 0]
insns = {0x1000+i*4: decode(w, 0x1000+i*4) for i,w in enumerate(words)}
fn = SimpleNamespace(entry=0x1000, addrs=[0x1000,0x1008,0x100C,0x1010,0x1014], labels={0x1008})
program = SimpleNamespace(insns=insns, names={}, functions={0x1000:fn}, switches={}, in_text=lambda a: a in insns)
harness += Emitter(program).emit_function(fn)
harness += r'''
int main(void) {
    static unsigned char ram[65536];
    for (unsigned p=0;p<sizeof(ram)/PS2_PAGE_SIZE;p++) ps2_pt[p]=ram+p*PS2_PAGE_SIZE;
    /* Segment crosses the interior of a triangle in y=0; its normal is +Y. */
    float vectors[6][4] = {
        {0,1,0,1}, {0,-1,0,1},
        {-10,0,-10,1}, {0,0,10,1}, {10,0,-10,1}, {0,1,0,0}
    };
    ps2_ctx ctx = {0};
    ctx.vu0.vf[0].f[3] = 1;
    ctx.r[29].uw[0] = 4096;
    ctx.r[4].uw[0] = 256;
    for (int i=0;i<6;i++) {
        memcpy(ram+512+i*16, vectors[i],16);
        ctx.r[5+i].uw[0] = 512+i*16;
    }
    func_001A3B38(&ctx);
    printf("Actual generated segment/triangle routine: hit=%u (geometric expectation 1)\n",ctx.r[2].uw[0]);
    int bad = ctx.r[2].uw[0] != 1;
    /* Outside triangle, then no crossing of its plane. */
    vectors[0][0]=vectors[1][0]=100;
    for (int i=0;i<6;i++) memcpy(ram+512+i*16,vectors[i],16);
    ctx.r[29].uw[0]=4096; func_001A3B38(&ctx);
    bad += ctx.r[2].uw[0] != 0;
    vectors[0][0]=vectors[1][0]=0; vectors[1][1]=2;
    for (int i=0;i<6;i++) memcpy(ram+512+i*16,vectors[i],16);
    ctx.r[29].uw[0]=4096; func_001A3B38(&ctx);
    bad += ctx.r[2].uw[0] != 0;
    memset(&ctx,0,sizeof ctx);
    ctx.vu0.vf[1].f[0]=-1;
    ps2_vu0_sub(&ctx,8,0,1,0);
    ps2_vu0_advance(&ctx,4); bad += ctx.vu0.mac != 0;
    ps2_vu0_advance(&ctx,1); bad += ctx.vu0.mac != 0x80;
    bad += (ctx.vu0.status & 0x82) != 0x82;
    ps2_vu0_sub(&ctx,8,0,1,1);
    ps2_vu0_advance(&ctx,5); bad += ctx.vu0.mac != 8;
    bad += (ctx.vu0.status & 0xC3) != 0xC1; /* sticky sign survives zero */
    ps2_vu0_ctc(&ctx,16,0); bad += ctx.vu0.status != 1;
    ps2_vu0_max(&ctx,8,2,1,0); ps2_vu0_advance(&ctx,5);
    bad += ctx.vu0.mac != 8; /* MAX does not affect flags */
    ctx.vu0.vf[1].f[0]=0x1p-126f; ctx.vu0.vf[2].f[0]=0.5f;
    ps2_vu0_mul(&ctx,8,3,1,2); ps2_vu0_advance(&ctx,5);
    bad += ctx.vu0.mac != 0x808; /* zero + underflow */
    for (int taken=0;taken<2;taken++) {
        memset(&ctx,0,sizeof ctx);
        ctx.vu0.vf[1].f[0]=-1;
        ps2_vu0_sub(&ctx,8,0,1,0);
        ctx.r[4].uw[0]=taken?0:1;
        func_00001000(&ctx);
        bad += ctx.r[2].uw[0] != (taken?0x80u:0u);
    }
    /* Real mission cleanup wrapper. It must obtain the manager from GP,
     * move the incoming object into a1, then enter its lower-address loop. */
    memset(ram,0,sizeof ram); memset(&ctx,0,sizeof ctx);
    ctx.r[29].uw[0]=4096; ctx.r[28].uw[0]=0x3000;
    ctx.r[4].uw[0]=0x1000;
    ps2_w32(0x3000-0x2740,0x4000);
    ps2_w32(0x4000+0x1400,1);
    ps2_w32(0x4000+0xA00,123);
    ps2_w32(0x4000+0xA08,77);
    ps2_w32(0x4000,0xDEADBEEF);
    ps2_w32(0x1008,77);
    old_cleanup_entry(&ctx);
    int reproduced=ps2_r32(0x4A00)==123 && ps2_r32(0x4000)==0xDEADBEEF;
    printf("Old cleanup-entry defect reproduced: %s\n",reproduced?"YES":"NO");
    bad+=!reproduced;
    ctx.r[4].uw[0]=0x1000;
    func_00356C38(&ctx);
    int cleanup_bad=ps2_r32(0x4A00)!=0xFFFFFFFFu || ps2_r32(0x4000)!=0
                    || ctx.r[29].uw[0]!=4096;
    printf("Actual generated mission cleanup entry regression: %s\n",cleanup_bad?"FAIL":"PASS");
    bad+=cleanup_bad;
    printf("Runtime regressions: %s\n",bad?"FAIL":"PASS");
    return bad ? 1 : 0;
}
'''
(out/'collision_probe.c').write_text(harness)
cmd = ['gcc','-std=gnu2x','-O1','-ffunction-sections','-fdata-sections',
       '-fno-strict-aliasing','-fwrapv',
       '-I'+str(root/'runtime/include'),str(out/'collision_probe.c'),
       str(root/'runtime/src/ps2_core.c'),'-Wl,--gc-sections','-lm',
       '-o',str(out/'collision_probe.exe')]
subprocess.run(cmd,check=True)
subprocess.run([str(out/'collision_probe.exe')],check=True)
