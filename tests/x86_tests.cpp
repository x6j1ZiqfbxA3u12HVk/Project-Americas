// Hand-assembled encodings, each checked for length, operand layout, flow and required feature.

#include <initializer_list>
#include <vector>

#include "test.h"
#include "x86/decoder.h"

using namespace am::x86;
using am::host::CpuFeature;

namespace {

struct Decoded {
  DecodeStatus status;
  Instruction insn;
};

Decoded dec(std::initializer_list<uint8_t> bytes) {
  const std::vector<uint8_t> code(bytes);
  Decoded d{};
  d.status = decode(code, d.insn);
  return d;
}

bool len(std::initializer_list<uint8_t> bytes, uint8_t expected) {
  const Decoded d = dec(bytes);
  return d.status == DecodeStatus::Ok && d.insn.length == expected;
}

std::optional<CpuFeature> feature(std::initializer_list<uint8_t> bytes) {
  const Decoded d = dec(bytes);
  if (d.status != DecodeStatus::Ok) return std::nullopt;
  return requirement(d.insn).feature;
}

bool not_on_console(std::initializer_list<uint8_t> bytes) {
  const Decoded d = dec(bytes);
  return d.status == DecodeStatus::Ok && requirement(d.insn).not_on_console;
}

}  // namespace

TEST_CASE(x86_lengths_of_common_integer_code) {
  CHECK(len({0x90}, 1));                                            // nop
  CHECK(len({0xC3}, 1));                                            // ret
  CHECK(len({0x48, 0x89, 0x5C, 0x24, 0x08}, 5));                    // mov [rsp+8], rbx
  CHECK(len({0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}, 7));        // sub rsp, 0x100
  CHECK(len({0x66, 0x81, 0xFA, 0x34, 0x12}, 5));                    // cmp dx, 0x1234
  CHECK(len({0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}, 10));             // mov rax, imm64
  CHECK(len({0x66, 0xB8, 0x34, 0x12}, 4));                          // mov ax, imm16
  CHECK(len({0xB8, 0x78, 0x56, 0x34, 0x12}, 5));                    // mov eax, imm32
  CHECK(len({0xF6, 0xC1, 0x01}, 3));                                // test cl, 1
  CHECK(len({0xF6, 0xD8}, 2));                                      // neg al
  CHECK(len({0xF7, 0xC1, 1, 0, 0, 0}, 6));                          // test ecx, imm32
  CHECK(len({0xF7, 0xD8}, 2));                                      // neg eax
  CHECK(len({0xC8, 0x10, 0x00, 0x01}, 4));                          // enter 16, 1
  CHECK(len({0xA1, 1, 2, 3, 4, 5, 6, 7, 8}, 9));                    // mov eax, [moffs64]
  CHECK(len({0x67, 0xA1, 1, 2, 3, 4}, 6));                          // mov eax, [moffs32]
  CHECK(len({0x4C, 0x8B, 0x04, 0xC1}, 4));                          // mov r8, [rcx+rax*8]
  CHECK(len({0x8B, 0x04, 0x25, 0x78, 0x56, 0x34, 0x12}, 7));        // mov eax, [abs32]
  CHECK(len({0x0F, 0x1F, 0x44, 0x00, 0x00}, 5));                    // nop dword [rax+rax]
  CHECK(len({0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 9));
  CHECK(len({0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 11));
  CHECK(len({0x8F, 0xC0}, 2));                                      // pop rax (not XOP)
  CHECK(len({0x8F, 0x05, 1, 2, 3, 4}, 6));                          // pop [rip+disp32]
  CHECK(len({0x66, 0x0F, 0x6F, 0x05, 1, 2, 3, 4}, 8));              // movdqa xmm0, [rip+disp32]
  CHECK(len({0xC6, 0xF8, 0x05}, 3));                                // xabort 5
  // A REX prefix followed by a legacy prefix is ignored: 66 then decides the immediate size.
  CHECK(len({0x48, 0x66, 0xB8, 0x34, 0x12}, 5));
}

TEST_CASE(x86_operand_layout) {
  const Decoded d = dec({0x48, 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12});  // mov rax, [rip+0x12345678]
  CHECK(d.status == DecodeStatus::Ok);
  CHECK(d.insn.length == 7);
  CHECK(d.insn.rip_relative);
  CHECK(d.insn.rex == 0x48);
  CHECK(d.insn.modrm_offset == 2);
  CHECK(d.insn.disp_offset == 3 && d.insn.disp_size == 4);
  CHECK(d.insn.imm_size == 0);

  const Decoded e = dec({0x66, 0x0F, 0x78, 0xC1, 0x04, 0x08});  // extrq xmm1, 4, 8
  CHECK(e.status == DecodeStatus::Ok);
  CHECK(e.insn.length == 6);
  CHECK(e.insn.imm_offset == 4 && e.insn.imm_size == 2);
}

TEST_CASE(x86_control_flow) {
  Decoded d = dec({0xE8, 0x00, 0x00, 0x00, 0x00});
  CHECK(d.insn.flow == Flow::Call && d.insn.branch_disp == 0 && d.insn.length == 5);
  d = dec({0x0F, 0x84, 0x10, 0x00, 0x00, 0x00});
  CHECK(d.insn.flow == Flow::CondJump && d.insn.branch_disp == 16 && d.insn.length == 6);
  d = dec({0x74, 0xFE});
  CHECK(d.insn.flow == Flow::CondJump && d.insn.branch_disp == -2);
  d = dec({0xEB, 0x05});
  CHECK(d.insn.flow == Flow::Jump && d.insn.branch_disp == 5);
  d = dec({0xE9, 0xFB, 0xFF, 0xFF, 0xFF});
  CHECK(d.insn.flow == Flow::Jump && d.insn.branch_disp == -5);
  d = dec({0xE2, 0xFE});
  CHECK(d.insn.flow == Flow::CondJump);  // loop
  d = dec({0xFF, 0xE0});
  CHECK(d.insn.flow == Flow::IndirectJump && d.insn.length == 2);  // jmp rax
  d = dec({0xFF, 0x15, 1, 2, 3, 4});
  CHECK(d.insn.flow == Flow::IndirectCall && d.insn.rip_relative && d.insn.length == 6);
  d = dec({0x41, 0xFF, 0xD3});
  CHECK(d.insn.flow == Flow::IndirectCall && d.insn.length == 3);  // call r11
  d = dec({0xFF, 0x25, 1, 2, 3, 4});
  CHECK(d.insn.flow == Flow::IndirectJump);
  d = dec({0xC7, 0xF8, 0x10, 0x00, 0x00, 0x00});
  CHECK(d.insn.flow == Flow::CondJump && d.insn.length == 6 && d.insn.branch_disp == 16);  // xbegin
  CHECK(dec({0xC3}).insn.flow == Flow::Return);
  CHECK(dec({0xC2, 0x08, 0x00}).insn.flow == Flow::Return);
  CHECK(dec({0xCC}).insn.flow == Flow::Stop);
  CHECK(dec({0x0F, 0x0B}).insn.flow == Flow::Stop);
  CHECK(dec({0xCD, 0x29}).insn.flow == Flow::Stop);  // fast fail
  CHECK(dec({0xCD, 0x2C}).insn.flow == Flow::Next);
}

TEST_CASE(x86_vex_evex_xop) {
  CHECK(len({0xC5, 0xF8, 0x77}, 3));                          // vzeroupper
  CHECK(len({0xC5, 0xFC, 0x28, 0xC1}, 4));                    // vmovaps ymm0, ymm1
  CHECK(len({0xC4, 0xE3, 0x7D, 0x18, 0xC1, 0x01}, 6));        // vinsertf128 ymm0, ymm0, xmm1, 1
  CHECK(len({0xC4, 0xE2, 0x7D, 0x58, 0xC0}, 5));              // vpbroadcastd ymm0, xmm0
  CHECK(len({0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1}, 6));        // vmovaps zmm0, zmm1
  CHECK(len({0x8F, 0xE8, 0x78, 0xC0, 0xC1, 0x05}, 6));        // XOP map 8 with imm8
  CHECK(dec({0x66, 0xC5, 0xF8, 0x77}).status == DecodeStatus::Invalid);  // no 66 before VEX
  CHECK(dec({0x48, 0xC4, 0xE2, 0x7D, 0x58, 0xC0}).status == DecodeStatus::Invalid);  // nor REX
}

TEST_CASE(x86_invalid_and_truncated) {
  CHECK(dec({0x06}).status == DecodeStatus::Invalid);  // push es
  CHECK(dec({0x62}).status == DecodeStatus::Truncated);
  CHECK(dec({0x48, 0x8B, 0x05, 0x78}).status == DecodeStatus::Truncated);
  CHECK(dec({0x0F}).status == DecodeStatus::Truncated);
  CHECK(dec({0xFF, 0xF8}).status == DecodeStatus::Invalid);  // FF /7
  CHECK(dec({0xC6, 0xC8, 0x00}).status == DecodeStatus::Invalid);  // C6 /1
  CHECK(dec({0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}).status ==
        DecodeStatus::Ok);  // 15 bytes is the limit
  CHECK(dec({0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}).status ==
        DecodeStatus::Invalid);
}

TEST_CASE(x86_zen2_only_features) {
  CHECK(feature({0x66, 0x0F, 0x78, 0xC1, 0x04, 0x08}) == CpuFeature::SSE4A);  // extrq imm
  CHECK(feature({0x66, 0x0F, 0x79, 0xCA}) == CpuFeature::SSE4A);              // extrq xmm
  CHECK(feature({0xF2, 0x0F, 0x79, 0xCA}) == CpuFeature::SSE4A);              // insertq
  CHECK(feature({0xF3, 0x0F, 0x2B, 0x08}) == CpuFeature::SSE4A);              // movntss [rax], xmm1
  CHECK(feature({0xF2, 0x0F, 0x2B, 0x08}) == CpuFeature::SSE4A);              // movntsd
  CHECK(!feature({0x0F, 0x2B, 0x08}).has_value());                            // movntps: SSE
  CHECK(!feature({0x0F, 0x78, 0xC1}).has_value());                            // vmread
  CHECK(feature({0x66, 0x0F, 0xAE, 0x30}) == CpuFeature::CLWB);
  CHECK(feature({0x66, 0x0F, 0xAE, 0x38}) == CpuFeature::CLFLUSHOPT);
  CHECK(!feature({0x0F, 0xAE, 0x38}).has_value());                            // clflush: SSE2
  CHECK(!feature({0x0F, 0xAE, 0xF0}).has_value());                            // mfence
  CHECK(feature({0x0F, 0x01, 0xFC}) == CpuFeature::CLZERO);
  CHECK(feature({0x0F, 0x01, 0xFA}) == CpuFeature::MONITORX);
  CHECK(feature({0x0F, 0x01, 0xFB}) == CpuFeature::MONITORX);
  CHECK(feature({0x0F, 0x01, 0xFD}) == CpuFeature::RDPRU);
  CHECK(feature({0x0F, 0x01, 0xF9}) == CpuFeature::RDTSCP);
  CHECK(feature({0xF3, 0x0F, 0xC7, 0xF8}) == CpuFeature::RDPID);
  CHECK(feature({0x0F, 0xC7, 0xF8}) == CpuFeature::RDSEED);
  CHECK(feature({0x0F, 0xC7, 0xF0}) == CpuFeature::RDRAND);
  CHECK(feature({0x48, 0x0F, 0xC7, 0x0E}) == CpuFeature::CX16);
  CHECK(feature({0x0F, 0x38, 0xC9, 0xC1}) == CpuFeature::SHA);
  CHECK(feature({0x0F, 0x3A, 0xCC, 0xC1, 0x01}) == CpuFeature::SHA);
}

TEST_CASE(x86_other_features) {
  CHECK(feature({0x66, 0x0F, 0x38, 0xDC, 0xC1}) == CpuFeature::AES);
  CHECK(feature({0x66, 0x0F, 0x3A, 0x44, 0xC1, 0x00}) == CpuFeature::PCLMULQDQ);
  CHECK(feature({0xF3, 0x0F, 0xB8, 0xC1}) == CpuFeature::POPCNT);
  CHECK(feature({0xF3, 0x0F, 0xBD, 0xC1}) == CpuFeature::LZCNT);
  CHECK(feature({0xF3, 0x0F, 0xBC, 0xC1}) == CpuFeature::BMI1);   // tzcnt
  CHECK(!feature({0x0F, 0xBC, 0xC1}).has_value());                // bsf
  CHECK(feature({0x0F, 0x38, 0xF0, 0x06}) == CpuFeature::MOVBE);
  CHECK(!feature({0xF2, 0x0F, 0x38, 0xF0, 0xC1}).has_value());    // crc32: SSE4.2
  CHECK(feature({0x66, 0x0F, 0x38, 0xF6, 0xC1}) == CpuFeature::ADX);
  CHECK(!feature({0x66, 0x0F, 0x38, 0x00, 0xC1}).has_value());    // pshufb: SSSE3
  CHECK(feature({0xC5, 0xFC, 0x28, 0xC1}) == CpuFeature::AVX);    // vmovaps ymm
  CHECK(feature({0xC5, 0xFD, 0x6F, 0xC1}) == CpuFeature::AVX);    // vmovdqa ymm: a move
  CHECK(feature({0xC5, 0xFD, 0xFE, 0xC1}) == CpuFeature::AVX2);   // vpaddd ymm
  CHECK(feature({0xC5, 0xF9, 0xFE, 0xC1}) == CpuFeature::AVX);    // vpaddd xmm
  CHECK(feature({0xC4, 0xE2, 0x7D, 0x58, 0xC0}) == CpuFeature::AVX2);  // vpbroadcastd
  CHECK(feature({0xC4, 0xE2, 0x75, 0xB8, 0xC2}) == CpuFeature::FMA3);  // vfmadd231ps ymm
  CHECK(feature({0xC4, 0xE2, 0x79, 0x13, 0xC1}) == CpuFeature::F16C);  // vcvtph2ps
  CHECK(feature({0xC4, 0xE2, 0x70, 0xF2, 0xC2}) == CpuFeature::BMI1);  // andn
  CHECK(feature({0xC4, 0xE3, 0xFB, 0xF0, 0xC1, 0x05}) == CpuFeature::BMI2);  // rorx
  CHECK(feature({0xC4, 0xE3, 0xFD, 0x00, 0xC1, 0x4E}) == CpuFeature::AVX2);  // vpermq
  CHECK(not_on_console({0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1}));  // AVX-512
  CHECK(not_on_console({0x8F, 0xE8, 0x78, 0xC0, 0xC1, 0x05}));  // XOP
  CHECK(not_on_console({0x0F, 0x0F, 0xC1, 0x9E}));              // 3DNow!
}
