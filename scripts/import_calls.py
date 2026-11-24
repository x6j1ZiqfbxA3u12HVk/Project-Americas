"""Call sites of a PE32+ image's imports from chosen DLLs: direct calls through the import table
and calls or jumps to import thunks. Read-only.

usage: python -I scripts/import_calls.py <exe> <dll>[,<dll>...]"""
import mmap
import struct
import sys
from collections import defaultdict

path = sys.argv[1]
wanted_dlls = {d.lower() for d in sys.argv[2].split(',')}

f = open(path, 'rb')
m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
e_lfanew = struct.unpack_from('<I', m, 0x3c)[0]
nsec = struct.unpack_from('<H', m, e_lfanew + 6)[0]
opt_size = struct.unpack_from('<H', m, e_lfanew + 20)[0]
opt = e_lfanew + 24
image_base = struct.unpack_from('<Q', m, opt + 24)[0]
dirs = opt + 112
sects = []
so = opt + opt_size
for i in range(nsec):
    name = m[so:so + 8].rstrip(b'\0').decode()
    vsize, va, rsize, raw = struct.unpack_from('<IIII', m, so + 8)
    chars = struct.unpack_from('<I', m, so + 36)[0]
    sects.append((name, va, vsize, raw, rsize, chars))
    so += 40


def off(rva):
    for name, va, vsize, raw, rsize, chars in sects:
        if va <= rva < va + max(vsize, rsize):
            return raw + rva - va
    raise ValueError(hex(rva))


def cstr(rva):
    o = off(rva)
    e = m.find(b'\0', o)
    return m[o:e].decode()


imp_rva = struct.unpack_from('<I', m, dirs + 8)[0]
slots = {}  # IAT VA -> "dll!fn"
o = off(imp_rva)
while True:
    oft, ts, fc, name_rva, ft = struct.unpack_from('<IIIII', m, o)
    if name_rva == 0:
        break
    dll = cstr(name_rva).lower()
    if dll in wanted_dlls:
        lookup = oft or ft
        k = 0
        while True:
            v = struct.unpack_from('<Q', m, off(lookup) + 8 * k)[0]
            if v == 0:
                break
            fn = '#%d' % (v & 0xffff) if v >> 63 else cstr((v & 0x7fffffff) + 2)
            slots[image_base + ft + 8 * k] = dll + '!' + fn
            k += 1
    o += 20

thunks = {}
calls = defaultdict(list)
for name, va, vsize, raw, rsize, chars in sects:
    if not chars & 0x20000000:
        continue
    data = m[raw:raw + min(vsize, rsize)]
    base = image_base + va
    i = data.find(b'\xff\x25')
    while i >= 0:
        tgt = base + i + 6 + struct.unpack_from('<i', data, i + 2)[0]
        if tgt in slots:
            thunks[base + i] = slots[tgt]
        i = data.find(b'\xff\x25', i + 1)
    i = data.find(b'\xff\x15')
    while i >= 0:
        tgt = base + i + 6 + struct.unpack_from('<i', data, i + 2)[0]
        if tgt in slots:
            calls[slots[tgt]].append(base + i)
        i = data.find(b'\xff\x15', i + 1)
    i = data.find(b'\xe8')
    while i >= 0:
        if i + 5 <= len(data):
            tgt = base + i + 5 + struct.unpack_from('<i', data, i + 1)[0]
            if tgt in thunks:
                calls[thunks[tgt]].append(base + i)
        i = data.find(b'\xe8', i + 1)
    i = data.find(b'\xe9')
    while i >= 0:
        if i + 5 <= len(data):
            tgt = base + i + 5 + struct.unpack_from('<i', data, i + 1)[0]
            if tgt in thunks:
                calls[thunks[tgt] + ' (tail)'].append(base + i)
        i = data.find(b'\xe9', i + 1)

for fn in sorted(set(slots.values())):
    sites = calls.get(fn, []) + calls.get(fn + ' (tail)', [])
    print('%-50s %3d  %s' % (fn, len(sites), ' '.join('%x' % s for s in sorted(sites))))
