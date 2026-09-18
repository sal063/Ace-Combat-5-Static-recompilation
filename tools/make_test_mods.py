import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paths

root = Path(paths.ROOT)
out = root / 'out' / 'vfs_test'
exe = out / 'vfs_test.exe'
mods = root / 'tests' / 'mods'


def build():
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['gcc', '-std=gnu2x', '-O2', '-fno-strict-aliasing', '-fwrapv',
                    '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-I' + str(root / 'runtime/include'),
                    str(root / 'tools/vfs_test.c'),
                    str(root / 'runtime/src/ps2_vfs.c'),
                    '-o', str(exe)], check=True)


def cat(disc, path, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PS2_MOD_NAMES=str(root / 'config' / 'pac_names.txt'))
    subprocess.run([str(exe), 'cat', str(disc), path, str(dst)], check=True, env=env)


def manifest(mod, text):
    (mods / mod).mkdir(parents=True, exist_ok=True)
    (mods / mod / 'mod.toml').write_text(text, encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(prog='make_test_mods')
    ap.add_argument('--disc', default=paths.DISC,
                    help='the game, as an .iso or an extracted disc folder')
    ap.add_argument('--bgm', action='store_true',
                    help='also copy BIN/US/BGM.PAC (1.1 GB) for the whole-file test; '
                         'needs an extracted disc folder')
    args = ap.parse_args()
    disc = Path(args.disc)
    if not disc.exists():
        raise SystemExit('No disc at %s. Pass --disc.' % disc)
    build()

    manifest('mount_test', 'name     = "Mount Test"\nversion  = "1.0"\n')
    cat(disc, 'BIN/DATA.PAC/0000/select/noise.gim',
        mods / 'mount_test' / 'files' / 'select' / 'noise.gim')

    manifest('archive_test', 'name     = "Archive Test"\nversion  = "1.0"\n')
    files = mods / 'archive_test' / 'files'
    cat(disc, 'BIN/DATA.PAC/0003/dummy.bin', files / 'dummy.bin')
    cat(disc, 'BIN/DATA.PAC/0251/mis/mpl/mis.wpb', files / 'mis' / 'mpl' / 'mis.wpb')
    cat(disc, 'BIN/DATA.PAC/0126/#21', files / 'BIN' / 'DATA.PAC' / '0126' / '#21')

    resize = root / 'tests' / 'mods_resize' / 'resize_test' / 'files' / 'select' / 'noise.gim'
    resize.parent.mkdir(parents=True, exist_ok=True)
    data = (mods / 'mount_test' / 'files' / 'select' / 'noise.gim').read_bytes()
    resize.write_bytes(data + bytes(4096))

    if args.bgm:
        src = disc / 'BIN' / 'US' / 'BGM.PAC'
        if not src.is_file():
            raise SystemExit('--bgm needs an extracted disc folder with BIN/US/BGM.PAC in it')
        manifest('bgm_test', 'name     = "Music Test"\nversion  = "1.0"\n')
        dst = mods / 'bgm_test' / 'files' / 'BIN' / 'US' / 'BGM.PAC'
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        print('copied %s' % dst)
    print('test mods ready under %s' % mods)


if __name__ == '__main__':
    main()
