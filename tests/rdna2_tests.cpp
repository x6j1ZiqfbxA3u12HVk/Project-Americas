// The RDNA 2 decoder on hand-built instruction words, and the console program blobs on a
// synthetic container. The words follow the ISA's encodings (the corpus check is
// scripts/rdna2_oracle.py, against LLVM).

#include <cstring>
#include <vector>

#include "formats/inflate.h"
#include "shader/console_program.h"
#include "shader/rdna2.h"
#include "test.h"

namespace {

using namespace am::shader::rdna2;

Instruction one(std::vector<uint32_t> words, DecodeStatus expect = DecodeStatus::Ok) {
  Instruction insn;
  const DecodeStatus st = decode(words, 0, insn);
  CHECK(st == expect);
  return insn;
}

bool named(const Instruction& insn, const char* name) { return insn.info && std::strcmp(insn.info->name, name) == 0; }

}  // namespace

TEST_CASE(rdna2_scalar_and_literals) {
  CHECK(named(one({0xBF810000}), "s_endpgm") && one({0xBF810000}).dwords == 1);
  // s_mov_b32 s0, literal: SOP1 with ssrc0 = 255
  const Instruction mov = one({0xBE8003FF, 0x12345678});
  CHECK(named(mov, "s_mov_b32") && mov.dwords == 2 && mov.literal() == 0x12345678 && mov.field(Field::SDST) == 0);
  // s_add_u32 s1, s2, literal (SOP2, ssrc1 = 255)
  const Instruction add = one({0x8001FF02, 7});
  CHECK(named(add, "s_add_u32") && add.dwords == 2 && add.field(Field::SDST) == 1);
  // s_setreg_imm32_b32: its only form has a literal
  const Instruction setreg = one({0xBA80F804, 0x400});
  CHECK(named(setreg, "s_setreg_imm32_b32") && setreg.dwords == 2 && setreg.literal() == 0x400);
  // s_cbranch_scc0 +3
  const Instruction br = one({0xBF840003});
  CHECK(named(br, "s_cbranch_scc0") && (br.info->flags & kConditional) && br.field(Field::SIMM16) == 3);
  // s_load_dwordx4 s[0:3], s[2:3], 0x10 (SMEM, two dwords)
  const Instruction load = one({0xF4080001, 0xFA000010});
  CHECK(named(load, "s_load_dwordx4") && load.dwords == 2 && load.field(Field::OFFSET) == 0x10);
  one({}, DecodeStatus::Truncated);
  one({0xF4080001}, DecodeStatus::Truncated);
}

TEST_CASE(rdna2_vector_forms) {
  // v_add_f32 v1, v2, v3 (VOP2); with src0 = 255, a literal follows
  const Instruction add = one({0x06020702});
  CHECK(named(add, "v_add_f32") && add.dwords == 1 && add.field(Field::VDST) == 1 && add.field(Field::VSRC1) == 3);
  const Instruction lit = one({0x060206FF, 0x3F800000});
  CHECK(named(lit, "v_add_f32") && lit.dwords == 2 && lit.literal() == 0x3F800000);
  // SDWA (src0 = 249) and DPP16 (src0 = 250) take one more dword
  const Instruction sdwa = one({0x060206F9, 0x00060602});
  CHECK(named(sdwa, "v_add_f32") && sdwa.form == Form::Sdwa && sdwa.dwords == 2 && sdwa.field(Field::VSRC0) == 2);
  const Instruction dpp = one({0x7E0202FA, 0xFF00E402});
  CHECK(named(dpp, "v_mov_b32") && dpp.form == Form::Dpp16 && dpp.dwords == 2);
  // v_fmamk_f32: always a literal
  const Instruction fmamk = one({0x58020702, 0x40000000});
  CHECK(named(fmamk, "v_fmamk_f32") && fmamk.dwords == 2);
  // VOP3: v_fma_f32 v0, v1, v2, literal (src2 = 255)
  const Instruction fma = one({0xD54B0000, 0x03FE0501, 0x3F000000});
  CHECK(named(fma, "v_fma_f32") && fma.dwords == 3 && fma.literal() == 0x3F000000);
  // The compiler's hash marker: v_nop in VOP3 with src2 = 255 carries a literal.
  CHECK(named(one({0xD58062F1, 0x03FF5B5F, 0xEE658D4D}), "v_nop") && one({0xD58062F1, 0x03FF5B5F, 0xEE658D4D}).dwords == 3);
  // VOP3b: v_add_co_u32 has a scalar destination
  const Instruction co = one({0xD70F6A01, 0x00020502});
  CHECK(named(co, "v_add_co_u32") && co.field(Field::SDST) == 0x6A && co.field(Field::VDST) == 1);
}

TEST_CASE(rdna2_console_specifics) {
  // RDNA 1's v_mac_f32 (VOP2 31) and v_mad_f32 (VOP3 321): the console GPU keeps them. (PC RDNA 2
  // reads VOP2 31 as v_dot2c_f32_f16; the corpus also uses v_mad_f32 and v_madak_f32, which only
  // RDNA 1 has, so it is the MAD family.)
  const Instruction mac = one({0x3E020702});
  CHECK(named(mac, "v_mac_f32") && (mac.info->flags & kLegacy));
  CHECK(named(one({0xD5410000, 0x040E0501}), "v_mad_f32"));
  // MIMG's eighth opcode bit is bit 0: image_bvh_intersect_ray is 230.
  // (Words from LLVM's assembler: image_bvh[64]_intersect_ray v[4:7], v[0:10], s[8:11].)
  const Instruction bvh = one({0xF1989F01, 0x00020400});
  CHECK(named(bvh, "image_bvh_intersect_ray") && bvh.opcode == 230 && bvh.dwords == 2);
  CHECK(named(one({0xF19C9F01, 0x00020400}), "image_bvh64_intersect_ray"));
  // MIMG NSA: 1 to 3 more dwords of addresses
  CHECK(one({0xF0800F02, 0x00000100, 0x00000302}).dwords == 3);
  // MUBUF's eighth opcode bit is bit 25: buffer_load_format_d16_xyzw is 131.
  const Instruction d16 = one({0xE20C2000, 0x80092625});
  CHECK(named(d16, "buffer_load_format_d16_xyzw") && d16.opcode == 131);
}

TEST_CASE(rdna2_control_flow_scan) {
  const std::vector<uint32_t> code = {
      0xBF850002,  // 0: s_cbranch_scc1 +2 -> 3
      0x7E020280,  // 1: v_mov_b32 v1, 0
      0xBF810000,  // 2: s_endpgm
      0xBF820001,  // 3: s_branch +1 -> 5
      0x00000000,  // 4: data, never reached
      0xBF810000,  // 5: s_endpgm
      0xBF9F0000,  // 6: s_code_end padding
  };
  const ProgramScan scan = scan_program(code);
  CHECK(scan.errors == 0 && scan.ends && !scan.indirect);
  CHECK((scan.starts == std::vector<uint32_t>{0, 1, 2, 3, 5}));
  CHECK((scan.targets == std::vector<uint32_t>{3, 5}));
  const ProgramScan bad = scan_program(std::vector<uint32_t>{0xBF820010, 0xBF810000});  // branch past the end
  CHECK(bad.errors == 1 && bad.first_error == 0);
  const ProgramScan off_end = scan_program(std::vector<uint32_t>{0x7E020280});
  CHECK(off_end.errors == 1 && !off_end.ends);
}

namespace {

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); }
void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) { std::memcpy(v.data() + at, &x, 8); }

// A zlib stream of one stored block.
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> z = {0x78, 0x01, 0x01};
  const uint16_t len = static_cast<uint16_t>(data.size()), nlen = static_cast<uint16_t>(~len);
  z.insert(z.end(), {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(nlen),
                     static_cast<uint8_t>(nlen >> 8)});
  z.insert(z.end(), data.begin(), data.end());
  const uint32_t a = am::formats::adler32(data);
  z.insert(z.end(), {static_cast<uint8_t>(a >> 24), static_cast<uint8_t>(a >> 16), static_cast<uint8_t>(a >> 8),
                     static_cast<uint8_t>(a)});
  return z;
}

}  // namespace

TEST_CASE(console_program_blobs) {
  // FSSM payload: slot table, one compute record at 0x100, two dwords of code at 0x200.
  std::vector<uint8_t> payload(0x208, 0);
  put64(payload, 0, 6);
  for (size_t at = 0x08; at <= 0x60; at += 8) put64(payload, at, 0xDEADBEEFDEADBEEFull);
  put64(payload, 0x60, 0x100);
  put64(payload, 0x100, 0x200);
  put64(payload, 0x108, 8);
  put32(payload, 0x114, 1u << 5);               // wave32
  put32(payload, 0x14c, 4096);                  // LDS bytes
  put32(payload, 0x150, 24 | (10u << 20));      // VGPRs, SGPRs
  put32(payload, 0x200, 0xBF800000);            // s_nop 0
  put32(payload, 0x204, 0xBF810000);            // s_endpgm
  std::vector<uint8_t> fssm(0x10, 0);
  std::memcpy(fssm.data(), "FSSM", 4);
  put32(fssm, 4, 0x101);
  put64(fssm, 8, payload.size());
  fssm.insert(fssm.end(), payload.begin(), payload.end());
  const std::vector<uint8_t> packed = zlib_stored(fssm);
  std::vector<uint8_t> xbch(0x18, 0);
  std::memcpy(xbch.data(), "XBCH", 4);
  put32(xbch, 4, 3);
  put64(xbch, 8, packed.size());
  put64(xbch, 16, fssm.size());
  xbch.insert(xbch.end(), packed.begin(), packed.end());
  // DXIL part data: program header, XDAT index with one native entry, the blob.
  std::vector<uint8_t> part(0x30, 0);
  std::memcpy(part.data() + 0x18, "XDAT", 4);
  put32(part, 0x1c, 1);
  put32(part, 0x20, 0x24);
  put32(part, 0x24, 0x0d);
  put32(part, 0x28, static_cast<uint32_t>(part.size()));
  put32(part, 0x2c, static_cast<uint32_t>(xbch.size()));
  part.insert(part.end(), xbch.begin(), xbch.end());

  std::string error;
  const auto shader = am::shader::read_console_shader(part, error);
  CHECK(shader && shader->blobs.size() == 1 && shader->blobs[0].programs.size() == 1);
  if (!shader || shader->blobs.empty() || shader->blobs[0].programs.empty()) return;
  const am::shader::NativeBlob& blob = shader->blobs[0];
  const am::shader::NativeProgram& p = blob.programs[0];
  CHECK(p.stage == am::shader::HwStage::Cs && p.wave32 && p.vgprs == 24 && p.sgprs == 10 && p.lds_bytes == 4096);
  CHECK(blob.code(p).size() == 8);
  // A wrong version is refused.
  std::vector<uint8_t> bad = part;
  put32(bad, 0x30 + 4, 4);
  CHECK(!am::shader::read_console_shader(bad, error) && error.find("XBCH version") != std::string::npos);
}
