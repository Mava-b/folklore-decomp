#!/usr/bin/env python3
"""Guest setjmp/longjmp carried out on the host stack (runtime/ppu/ppu_sjlj.c).

Builds the real runtime/ppu/ppu_loader.cpp and ppu_sjlj.c with
runtime/ppu/tests/test_ppu_sjlj.cpp, a guest program written the way
ppu_lifter.py --sjlj lifts one: a protected call whose setjmp is a host setjmp
site and a throw whose longjmp jumps to it -- from deep inside indirect calls,
from nested protected calls, through one reused jmp_buf, and (refused, the
longjmp returns as before) through a stale jmp_buf and from another guest
context.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import stub_header  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses the POSIX scaffold stubs')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-sjlj-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        inc = ['-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
               '-I' + str(ROOT / 'runtime/ppu')]
        obj = tmp / 'ppu_sjlj.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-c', *inc,
                        str(ROOT / 'runtime/ppu/ppu_sjlj.c'), '-o', str(obj)], check=True)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-w',
                        '-ffunction-sections', '-fdata-sections', *inc,
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_sjlj.cpp'), str(obj),
                        *flags, '-o', str(binary)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: guest setjmp/longjmp on the host stack')
        print('PASS: guest longjmp lands in its setjmp site; stale and cross-context ones return')


if __name__ == '__main__':
    main()
