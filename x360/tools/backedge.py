"""JSRF: acrescenta X86_BACKEDGE() antes de cada salto para tras do codigo gerado, para que um laco que espera outra
thread do jogo possa ceder o bloqueio (kernel/guestlock.cpp). Mesma ideia de --back-edge-yield do Unleashed.
  python backedge.py <pasta do codigo gerado>
"""
import os
import re
import sys

COMMENT = re.compile(r'^\s*// ([0-9A-F]{8}) ')
GOTO = re.compile(r'goto loc_([0-9A-F]{8});')
total = 0
# MC3: MC3_DIAG=1 also records the last loop and the function entered (X86_BACKEDGE_AT, X86_FUNC_ENTER) for the
# profiler; off by default, as every call and loop pays for it (2026-11-11)
DIAG = os.environ.get('MC3_DIAG') == '1'
for name in sorted(os.listdir(sys.argv[1])):
    if not re.match(r'x86_recomp\.\d+\.cpp$', name):
        continue
    path = os.path.join(sys.argv[1], name)
    out = []
    cur = 0
    with open(path, encoding='utf-8', newline='') as f:
        for line in f:
            m = COMMENT.match(line)
            if m:
                cur = int(m.group(1), 16)
            elif DIAG and line.startswith('X86_FUNC(__imp__sub_') and line.rstrip().endswith('{'):
                # MC3 diagnostic: remember the function entered, the profiler prints it (2026-11-11)
                out.append(line)
                out.append('\tX86_FUNC_ENTER(0x%s);\n' % line[len('X86_FUNC(__imp__sub_'):len('X86_FUNC(__imp__sub_') + 8])
                continue
            elif 'goto loc_' in line:
                def fix(g):
                    global total
                    if int(g.group(1), 16) > cur:
                        return g.group(0)
                    total += 1
                    if DIAG:
                        return '{ X86_BACKEDGE_AT(0x%X); %s }' % (cur, g.group(0))
                    return '{ X86_BACKEDGE(); %s }' % g.group(0)
                line = GOTO.sub(fix, line)
            out.append(line)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(out)
print("%d saltos para tras com %s" % (total, "X86_BACKEDGE_AT" if DIAG else "X86_BACKEDGE"))
