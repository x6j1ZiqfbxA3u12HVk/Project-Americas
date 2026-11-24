"""Names the americas_rt.dll+0x... frames of logs\hang.txt or crash.txt from the linker map.

usage: python -I scripts/symbolize.py <build>\bin\americas_rt.map <report>"""
import bisect
import re
import sys

map_path, report = sys.argv[1], sys.argv[2]
base = None
syms = []
for line in open(map_path, encoding='utf-8', errors='replace'):
    m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
    if m:
        base = int(m.group(1), 16)
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
    if m and base is not None:
        syms.append((int(m.group(2), 16) - base, m.group(1)))
syms.sort()
rvas = [r for r, _ in syms]
for line in open(report, encoding='utf-8', errors='replace'):
    line = line.rstrip('\n')
    m = re.search(r'americas_rt\.dll\+0x([0-9a-f]+)', line)
    if m:
        rva = int(m.group(1), 16)
        i = bisect.bisect_right(rvas, rva) - 1
        if i >= 0:
            line += '   ' + syms[i][1] + '+0x%x' % (rva - syms[i][0])
    print(line)
