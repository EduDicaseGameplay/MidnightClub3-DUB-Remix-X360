"""MC3: alternates ice_loop.py (functions that crash cl go to [xbe] noopt) and x360/tools/ltcg_loop.py (files that crash
the /LTCG code generation leave /GL) until default.xex links. A file taken out of /GL can then hit a plain compiler
crash in one function, which only ice_loop handles, hence the alternation. Usage: python -I build_all.py [rounds]"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
for r in range(int(sys.argv[1]) if len(sys.argv) > 1 else 10):
    a = subprocess.run([sys.executable, '-I', os.path.join(here, 'ice_loop.py')], cwd=here)
    print('round', r, 'ice_loop', a.returncode, flush=True)
    if a.returncode:
        sys.exit(1)
    b = subprocess.run([sys.executable, '-I', os.path.join(here, 'x360', 'tools', 'ltcg_loop.py'), '20'], cwd=here,
                       capture_output=True, text=True)
    print(b.stdout[-1500:], flush=True)
    if b.returncode == 0:
        print('default.xex linked', flush=True)
        sys.exit(0)
    if 'build_gen falhou' not in b.stdout:
        sys.exit(1)
sys.exit(1)
