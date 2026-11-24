"""Generates src/shader/rdna2_tables.inl from AMD's machine-readable RDNA 2 ISA specification
(amdgpu_isa_rdna2.xml, MIT licence; kept in C:\\Project Americas\\tools\\amd-isa-xml, never in
the repository). The output carries AMD's copyright and licence notice.

For each instruction and each base encoding it has: the opcode, the name, its flags, and its
operands as the specification lists them. Literal, SDWA, DPP and NSA forms are not separate
entries: the decoder recognizes them from the base instruction's fields. An instruction whose
only form takes a literal (s_setreg_imm32_b32, v_madmk_f32, ...) is marked as always having one.

usage: python -I scripts/gen_rdna2_tables.py <amdgpu_isa_rdna2.xml> [<amdgpu_isa_rdna1.xml>] [<out.inc>]

With the RDNA 1 specification, opcodes RDNA 2 does not have are filled from it, flagged kLegacy:
the console's GPU still runs RDNA 1's v_mac_f32, v_mad_f32, v_madmk_f32 and v_madak_f32.
"""
import os
import sys
import xml.etree.ElementTree as ET

# Our encodings, in the order of the decoder's enum. VOP3_SDST_ENC shares VOP3's opcode space.
BASE = {
    'ENC_SOP1': 'Sop1', 'ENC_SOPC': 'Sopc', 'ENC_SOPP': 'Sopp', 'ENC_SOPK': 'Sopk', 'ENC_SOP2': 'Sop2',
    'ENC_SMEM': 'Smem', 'ENC_VOP1': 'Vop1', 'ENC_VOPC': 'Vopc', 'ENC_VOP2': 'Vop2', 'ENC_VINTRP': 'Vintrp',
    'ENC_VOP3': 'Vop3', 'VOP3_SDST_ENC': 'Vop3', 'ENC_VOP3P': 'Vop3p', 'ENC_DS': 'Ds', 'ENC_MUBUF': 'Mubuf',
    'ENC_MTBUF': 'Mtbuf', 'ENC_MIMG': 'Mimg', 'ENC_EXP': 'Exp', 'ENC_FLAT': 'Flat', 'ENC_FLAT_GLBL': 'FlatGlobal',
    'ENC_FLAT_SCRATCH': 'FlatScratch',
    # Forms that exist only with a literal.
    'SOPK_INST_LITERAL': 'Sopk', 'VOP2_INST_LITERAL': 'Vop2',
}
ALWAYS_LITERAL = {'SOPK_INST_LITERAL', 'VOP2_INST_LITERAL'}

LICENCE = """\
// This file is generated from AMD's machine-readable GPU ISA specification and carries its notice:
//
// {copyright}
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""


def collect(root, entries, fields, types, legacy):
    """Adds the instructions of one specification; with `legacy`, only opcodes still free."""
    for ins in root.find('ISA/Instructions').findall('Instruction'):
        name = ins.findtext('InstructionName').strip().lower()
        fl = ins.find('InstructionFlags')
        flags = []
        for tag, flag in (('IsBranch', 'kBranch'), ('IsConditionalBranch', 'kConditional'),
                          ('IsIndirectBranch', 'kIndirect'), ('IsProgramTerminator', 'kTerminator'),
                          ('IsImmediatelyExecuted', 'kImmediate')):
            if fl.findtext(tag) == 'TRUE':
                flags.append(flag)
        if legacy:
            flags.append('kLegacy')
        for enc in ins.find('InstructionEncodings').findall('InstructionEncoding'):
            ename = enc.findtext('EncodingName')
            cond = enc.findtext('EncodingCondition')
            if ename not in BASE:
                continue
            if ename in ALWAYS_LITERAL and cond != 'default':
                continue
            ours = BASE[ename]
            opcode = int(enc.findtext('Opcode'))
            key = (ours, opcode)
            if legacy and key in entries and not entries[key][3]:
                continue
            eflags = list(flags)
            if ename in ALWAYS_LITERAL:
                eflags.append('kLiteral')
            if ename == 'VOP3_SDST_ENC':
                eflags.append('kSdst')
            operands = []
            ops = enc.find('Operands')
            for o in (ops.findall('Operand') if ops is not None else []):
                field = (o.findtext('FieldName') or '').strip() or 'Implicit'
                otype = (o.findtext('OperandType') or 'OPR_NONE').strip()
                size = int(o.findtext('OperandSize') or 0)
                of = []
                if o.get('Input') == 'true':
                    of.append('kIn')
                if o.get('Output') == 'true':
                    of.append('kOut')
                if o.get('IsImplicit') == 'true':
                    of.append('kImplicit')
                fields.add(field)
                types.add(otype)
                operands.append((field, otype, size, '|'.join(of) or '0'))
            prev = entries.get(key)
            if prev and prev[0] != name:
                raise SystemExit('two instructions for %s opcode %d: %s, %s' % (ours, opcode, prev[0], name))
            if not prev or ename.startswith('ENC_') or ename == 'VOP3_SDST_ENC':
                entries[key] = (name, '|'.join(eflags) or '0', operands, legacy)


def main():
    xml_path = sys.argv[1]
    legacy_path = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2].endswith('.xml') else None
    rest = sys.argv[3:] if legacy_path else sys.argv[2:]
    out = rest[0] if rest else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'shader', 'rdna2_tables.inl')
    root = ET.parse(xml_path).getroot()
    doc = root.find('Document')
    copyright_line = doc.findtext('Copyright').strip()
    release = doc.findtext('ReleaseDate').strip()
    arch = root.findtext('ISA/Architecture/ArchitectureName').strip()
    assert arch == 'AMD RDNA 2', arch

    fields, types = set(), set()
    entries = {}  # (encoding, opcode) -> entry
    collect(root, entries, fields, types, legacy=False)
    legacy_note = ''
    if legacy_path:
        old = ET.parse(legacy_path).getroot()
        assert old.findtext('ISA/Architecture/ArchitectureName').strip() == 'AMD RDNA 1'
        collect(old, entries, fields, types, legacy=True)
        legacy_note = ' and amdgpu_isa_rdna1.xml (release %s)' % old.findtext('Document/ReleaseDate').strip()

    layouts = []  # (encoding, field, first bit, bits) of every encoding the specification has
    for e in root.find('ISA/Encodings').findall('Encoding'):
        ename = e.findtext('EncodingName').strip()
        for fld in e.iter('Field'):
            rng = fld.find('BitLayout/Range')
            if rng is None:
                continue
            fname = fld.findtext('FieldName').strip()
            fields.add(fname)
            layouts.append((ename, fname, int(rng.findtext('BitOffset')), int(rng.findtext('BitCount'))))
    order = list(dict.fromkeys(BASE.values()))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// RDNA 2 instruction tables, generated by scripts/gen_rdna2_tables.py from\n')
        f.write('// amdgpu_isa_rdna2.xml (%s, release %s)%s.\n// Do not edit: rerun the script.\n//\n' % (arch, release, legacy_note))
        f.write(LICENCE.format(copyright=copyright_line))
        f.write('\n// clang-format off\n')
        f.write('#ifdef AM_RDNA2_FIELDS\n')
        for x in sorted(fields):
            f.write('AM_FIELD(%s)\n' % x)
        f.write('#endif\n#ifdef AM_RDNA2_TYPES\n')
        for x in sorted(types):
            f.write('AM_TYPE(%s)\n' % x)
        f.write('#endif\n#ifdef AM_RDNA2_LAYOUT_NAMES\n')
        for name in dict.fromkeys(l[0] for l in layouts):
            f.write('AM_LAYOUT_NAME(%s)\n' % name)
        f.write('#endif\n#ifdef AM_RDNA2_LAYOUTS\n')
        f.write('// AM_LAYOUT(encoding as the specification names it, field, first bit, bits)\n')
        for layout in layouts:
            f.write('AM_LAYOUT(%s, %s, %d, %d)\n' % layout)
        f.write('#endif\n#ifdef AM_RDNA2_INSTRUCTIONS\n')
        f.write('// AM_INSN(encoding, opcode, name, flags, operand count, operands...)\n')
        f.write('// AM_OP(field, type, bits, flags)\n')
        count = 0
        for enc in order:
            for (e, opcode), (name, flags, operands, _) in sorted(entries.items(), key=lambda kv: (kv[0][0], kv[0][1])):
                if e != enc:
                    continue
                ops = ', '.join('AM_OP(%s, %s, %d, %s)' % o for o in operands)
                f.write('AM_INSN(%s, %d, "%s", %s, %d%s)\n' % (enc, opcode, name, flags, len(operands),
                                                             (', ' + ops) if ops else ''))
                count += 1
        f.write('#endif\n')
    print('%d instruction entries, %d fields, %d operand types -> %s' % (count, len(fields), len(types),
                                                                         os.path.normpath(out)))


main()
