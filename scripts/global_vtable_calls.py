"""Where code loads an interface pointer from a global (mov reg, [rip+X]) and which vtable slot it
calls next through it. Read-only.

usage: python -I scripts/global_vtable_calls.py <exe> <global va hex>
"""
import mmap
import struct
import sys
from collections import defaultdict

exe, target = sys.argv[1], int(sys.argv[2], 16)
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
# mov r64, [rip+disp32]: REX.W (48/4C) 8B modrm(00 reg 101)
for rex in (b'\x48\x8b', b'\x4c\x8b'):
    i = data.find(rex)
    while i >= 0:
        modrm = data[i + 2] if i + 2 < len(data) else 0
        if (modrm & 0xC7) == 0x05 and i + 7 <= len(data):
            t = base + i + 7 + struct.unpack_from('<i', data, i + 3)[0]
            if t == target:
                after = data[i + 7:i + 7 + 80]
                slot = None
                for k in range(len(after) - 3):
                    b0, b1, b2 = after[k], after[k + 1], after[k + 2]
                    if b0 in (0x41,) and b1 == 0xff and (b2 & 0xf8) in (0x50, 0x90) and (b2 & 7) != 4:
                        slot = (after[k + 3] if (b2 & 0xc0) == 0x40 else struct.unpack_from('<i', after, k + 3)[0]) // 8
                        break
                    if b0 == 0xff and (b1 & 0xf8) in (0x50, 0x90) and (b1 & 7) != 4:
                        slot = (after[k + 2] if (b1 & 0xc0) == 0x40 else struct.unpack_from('<i', after, k + 2)[0]) // 8
                        break
                slots[slot].append(base + i)
        i = data.find(rex, i + 1)
for s in sorted(slots, key=lambda x: -1 if x is None else x):
    print('slot %4s  %3d loads  e.g. %s' % (s, len(slots[s]), ' '.join('%x' % a for a in slots[s][:5])))
