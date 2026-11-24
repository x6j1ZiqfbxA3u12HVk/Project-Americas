"""After each call of a getter function, find the first indirect call through a vtable register
and report its slot. Read-only. usage: vcalls.py <exe> <getter va hex>[,<va>...]"""
import mmap
import struct
import sys
from collections import Counter, defaultdict

exe = sys.argv[1]
getters = {int(x, 16) for x in sys.argv[2].split(',')}
f = open(exe, 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
e = struct.unpack_from('<I', m, 0x3c)[0]
opt = e + 24
ib = struct.unpack_from('<Q', m, opt + 24)[0]
n = struct.unpack_from('<H', m, e + 6)[0]
osz = struct.unpack_from('<H', m, e + 20)[0]
for i in range(n):
    b = opt + osz + 40 * i
    if m[b:b + 5] == b'.text':
        vs, va, rs, raw = struct.unpack_from('<IIII', m, b + 8)
data = m[raw:raw + min(vs, rs)]
base = ib + va
slots = defaultdict(list)
i = data.find(b'\xe8')
while i >= 0:
    if i + 5 <= len(data) and base + i + 5 + struct.unpack_from('<i', data, i + 1)[0] in getters:
        after = data[i + 5:i + 5 + 96]
        for k in range(len(after) - 4):
            b0, b1, b2 = after[k], after[k + 1], after[k + 2]
            # call [reg+disp8] / [reg+disp32], with or without REX.B
            if b0 == 0x41 and b1 == 0xff and (b2 & 0xf8) in (0x50, 0x90) and (b2 & 7) != 4:
                d = after[k + 3] if (b2 & 0xc0) == 0x40 else struct.unpack_from('<i', after, k + 3)[0]
                slots[d // 8].append(base + i)
                break
            if b0 == 0xff and (b1 & 0xf8) in (0x50, 0x90) and (b1 & 7) != 4:
                d = after[k + 2] if (b1 & 0xc0) == 0x40 else struct.unpack_from('<i', after, k + 2)[0]
                slots[d // 8].append(base + i)
                break
    i = data.find(b'\xe8', i + 1)
for s in sorted(slots):
    print('slot %3d  %3d calls  e.g. %s' % (s, len(slots[s]), ' '.join('%x' % a for a in slots[s][:4])))
