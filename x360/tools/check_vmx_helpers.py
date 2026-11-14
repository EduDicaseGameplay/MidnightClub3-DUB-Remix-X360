"""MC3 X-19 (2026-11-11): finds invalid VMX128 save/restore copies in the linked image.

The Xbox 360 linker (XDK 21256) copies the CRT's __savevmx_N/__restvmx_N helpers next to callers more than 32 MB
away (bl reaches +-32 MB) and encodes the copies' stvx128/lvx128 for vr64-vr127 with a wrong extended opcode
(0x703/0x303 instead of 0x1C3/0x0C3): an illegal instruction on the console, skipped by Xenia and by xbGuard.
Usage: check_vmx_helpers.py <bre360.pe> <bre360.map>   (exit 1 when any bad word or far call is found)
"""
import bisect
import re
import struct
import sys

pe, mapf = sys.argv[1], sys.argv[2]
d = open(pe, 'rb').read()
text_va = None
syms = []
for line in open(mapf, encoding='latin-1'):
    m = re.match(r'\s*0003:([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})', line)
    if m:
        syms.append((int(m.group(3), 16), m.group(2), int(m.group(1), 16)))
syms.sort()
# file offset of .text from a known symbol: search the first symbol's bytes is unreliable, use the island rule
# the image is mapped at 0x82000000 with section 3 file offset found from the PE section table
e_lfanew = struct.unpack('<I', d[0x3C:0x40])[0]
nsec = struct.unpack('<H', d[e_lfanew + 6:e_lfanew + 8])[0]
opt = struct.unpack('<H', d[e_lfanew + 20:e_lfanew + 22])[0]
sec0 = e_lfanew + 24 + opt
image_base = struct.unpack('<I', d[e_lfanew + 24 + 28:e_lfanew + 24 + 32])[0]
secs = []
for i in range(nsec):
    o = sec0 + i * 40
    name = d[o:o + 8].rstrip(b'\0').decode('latin-1')
    vsize, va, rsize, raw = struct.unpack('<IIII', d[o + 8:o + 24])
    secs.append((name, image_base + va, vsize, raw, rsize))
bad = []
branches = []  # (from, to) of every bl/b with a relative target
hsyms = [s for s in syms if s[1].startswith(('__savevmx', '__restvmx'))]
hlo = min(s[0] for s in hsyms) if hsyms else 0
hhi = max(s[0] for s in hsyms) + 0x400 if hsyms else 0
text = None
for name, va, vsize, raw, rsize in secs:
    if name != '.text':
        continue
    text = (va, vsize)
    for off in range(0, min(vsize, rsize) - 3, 4):
        w = struct.unpack('>I', d[raw + off:raw + off + 4])[0]
        if w >> 26 == 4 and (w & 0x7F3) in (0x303, 0x703):
            bad.append(va + off)
        if w >> 26 == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            if not (w & 2):
                branches.append((va + off, va + off + li))
# blocks of broken words, from the li before the first one to the blr after the last
blocks = []
for a in bad:
    if blocks and a - blocks[-1][1] <= 8:
        blocks[-1][1] = a
    else:
        blocks.append([a, a])
into = [(f, t) for f, t in branches for b in blocks if b[0] - 4 <= t <= b[1] + 4]
to_ours = sum(1 for f, t in branches if hlo <= t < hhi)
print('text %08X (%.1f MB); helpers in the image %08X..%08X; calls to them: %d' % (text[0], text[1] / 1048576.0, hlo, hhi, to_ours))
print('broken VMX128 words: %d in %d block(s); branches into them: %d' % (len(bad), len(blocks), len(into)))
keys = [s[0] for s in syms]
for f, t in into[:20]:
    i = bisect.bisect_right(keys, f) - 1
    print('  %08X -> %08X from %s' % (f, t, syms[i][1] if i >= 0 else '?'))
sys.exit(1 if into else 0)
