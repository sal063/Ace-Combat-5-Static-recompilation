from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'out' / 'patch_test'
out.mkdir(parents=True, exist_ok=True)
exe = out / 'patch_test.exe'

subprocess.run(['gcc', '-std=gnu2x', '-O1', '-fno-strict-aliasing', '-fwrapv',
                '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-I' + str(root / 'runtime/include'),
                str(root / 'tools/patch_test.c'),
                str(root / 'runtime/src/ps2_patch.c'),
                '-o', str(exe)], check=True)
subprocess.run([str(exe), str(out / 'patch_test.pnach')], check=True)
