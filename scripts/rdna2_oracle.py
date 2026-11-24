"""Checks the RDNA 2 decoder against LLVM's disassembler (llvm-mc with the AMDGPU target).

For a sample of the corpus, every instruction our decoder reaches by control flow is given to
llvm-mc on its own line, followed by `s_nop <n>` as a numbered delimiter. Agreement means LLVM
decodes exactly one instruction of the same length with the same name (LLVM's _e32, _e64,
_sdwa and _dpp suffixes aside). The game's 24-byte hash markers (v_nop with a literal) are
skipped: LLVM refuses them. Each line is tried as RDNA 2 (gfx1030) and, for what RDNA 2 lacks,
as RDNA 1 with ray tracing (gfx1013). Delimiters have bit 15 set, which the program's own s_nop never has.

usage: python -I scripts/rdna2_oracle.py <am-shaders.exe> <llvm-mc.exe> <pack dir> [<sample> [<seed>]]
"""
import collections
import glob
import os
import random
import re
import subprocess
import sys
import tempfile

SUFFIX = re.compile(r'_(e32|e64|sdwa|dpp8|dpp)$')


def ours(am_shaders, pack, name, raw_dir):
    out = subprocess.run([am_shaders, 'disasm', name, '--out', pack, '--raw', raw_dir], capture_output=True, text=True).stdout
    programs, current = [], None
    for line in out.splitlines():
        m = re.match(r'; (\w+) program (\d+):', line)
        if m:
            current = (os.path.join(raw_dir, '%s_%s.bin' % (m.group(1), m.group(2))), [])
            programs.append(current)
            continue
        m = re.match(r'([0-9a-f]{6}) (\d) (\S+)', line)
        if m and current:
            current[1].append((int(m.group(1), 16), int(m.group(2)), m.group(3), line))
    return programs


def llvm(llvm_mc, cpu, lines):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        for i, hexbytes in enumerate(lines):
            f.write(hexbytes + ' 0x%02x 0x%02x 0x80 0xbf\n' % (i & 0xff, 0x80 | (i >> 8) & 0x7f))
        path = f.name
    out = subprocess.run([llvm_mc, '--disassemble', '-triple=amdgcn', '-mcpu=' + cpu, '--show-encoding', path],
                         capture_output=True, text=True)
    os.unlink(path)
    groups = collections.defaultdict(list)
    current = []
    for line in out.stdout.splitlines():
        line = line.strip()
        m = re.match(r's_nop (0x[0-9a-f]+|\d+)\s*; encoding: \[0x..,0x..,0x80,0xbf\]', line)
        if m:
            value = int(m.group(1), 0)
            if value < 0x8000:  # the program's own s_nop
                current.append(line)
                continue
            groups[value & 0x7fff].append(current)
            current = []
        elif line and not line.startswith('.'):
            current.append(line)
    return groups


def parse(group_lines):
    insns = []
    for line in group_lines:
        m = re.match(r'(\S+).*; encoding: \[([^\]]*)\]', line)
        if m:
            insns.append((m.group(1), len(m.group(2).split(','))))
    return insns


def main():
    am_shaders, llvm_mc, pack = sys.argv[1:4]
    sample = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    random.seed(int(sys.argv[5]) if len(sys.argv) > 5 else 1)
    files = sorted(glob.glob(os.path.join(pack, '*.dxbc')))
    chosen = random.sample(files, min(sample, len(files)))
    raw_dir = tempfile.mkdtemp(prefix='rdna2_oracle_')
    items = []  # (our name, dwords, hex, where)
    for path in chosen:
        name = os.path.basename(path)[:16]
        for bin_path, insns in ours(am_shaders, pack, name, raw_dir):
            code = open(bin_path, 'rb').read()
            for offset, dwords, mnemonic, line in insns:
                if mnemonic == 'v_nop' and dwords == 3:
                    continue  # a hash marker
                b = code[offset:offset + 4 * dwords]
                items.append((mnemonic, dwords, ' '.join('0x%02x' % x for x in b), '%s+%06x' % (name, offset)))
    verdict = ['?'] * len(items)
    mismatches = collections.Counter()
    examples = {}
    for cpu in ('gfx1030', 'gfx1013'):
        todo = [i for i, v in enumerate(verdict) if v != 'ok']
        for start in range(0, len(todo), 30000):
            batch = todo[start:start + 30000]
            groups = llvm(llvm_mc, cpu, [items[i][2] for i in batch])
            for k, i in enumerate(batch):
                mnemonic, dwords, _, where = items[i]
                got = groups.get(k)
                insns = parse(got[0]) if got else []
                if len(insns) == 1 and insns[0][1] == 4 * dwords and SUFFIX.sub('', insns[0][0]) == mnemonic:
                    verdict[i] = 'ok'
                elif cpu == 'gfx1013':
                    llvm_says = ' + '.join('%s(%d)' % x for x in insns) or 'invalid'
                    key = '%s(%d) vs %s' % (mnemonic, dwords * 4, llvm_says)
                    mismatches[key] += 1
                    examples.setdefault(key, where)
    agree = verdict.count('ok')
    print('%d containers, %d instructions compared: %d agree (%.3f %%)' % (len(chosen), len(items), agree,
                                                                            100.0 * agree / max(1, len(items))))
    for key, n in mismatches.most_common(40):
        print('  %7d  %s   e.g. %s' % (n, key, examples[key]))


main()
