// The emulation of console-only instructions, on small code buffers we assemble by hand.
// On a CPU that has an instruction it runs natively, so these check the expected results
// either way.

#include <windows.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include "runtime/cpu_compat.h"
#include "test.h"

namespace {

using Fn = uint64_t (*)(uint64_t, uint64_t);

// Copies `bytes` (ending in RET) into executable memory.
Fn assemble(std::initializer_list<uint8_t> bytes) {
  void* p = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  const std::vector<uint8_t> code(bytes);
  std::memcpy(p, code.data(), code.size());
  FlushInstructionCache(GetCurrentProcess(), p, code.size());
  return reinterpret_cast<Fn>(p);
}

}  // namespace

TEST_CASE(cpu_emulation_insertq_immediate) {
  am::rt::install_cpu_emulation();
  // movq xmm0, rcx ; movq xmm1, rdx ; insertq xmm0, xmm1, 8, 16 ; movq rax, xmm0 ; ret
  const Fn f = assemble({0x66, 0x48, 0x0F, 0x6E, 0xC1, 0x66, 0x48, 0x0F, 0x6E, 0xCA, 0xF2, 0x0F, 0x78, 0xC1, 0x08, 0x10,
                         0x66, 0x48, 0x0F, 0x7E, 0xC0, 0xC3});
  CHECK(f(0x1122334455667788ull, 0xABull) == 0x1122334455AB7788ull);
  CHECK(f(~0ull, 0) == 0xFFFFFFFFFF00FFFFull);
}

TEST_CASE(cpu_emulation_insertq_full_width) {
  am::rt::install_cpu_emulation();
  // insertq xmm0, xmm1, 0 (= 64 bits), 0: xmm0[63:0] = xmm1[63:0]
  const Fn f = assemble({0x66, 0x48, 0x0F, 0x6E, 0xC1, 0x66, 0x48, 0x0F, 0x6E, 0xCA, 0xF2, 0x0F, 0x78, 0xC1, 0x00, 0x00,
                         0x66, 0x48, 0x0F, 0x7E, 0xC0, 0xC3});
  CHECK(f(1, 0xFEDCBA9876543210ull) == 0xFEDCBA9876543210ull);
}

TEST_CASE(cpu_emulation_extrq_immediate) {
  am::rt::install_cpu_emulation();
  // movq xmm0, rcx ; extrq xmm0, 16, 8 ; movq rax, xmm0 ; ret
  const Fn f = assemble({0x66, 0x48, 0x0F, 0x6E, 0xC1, 0x66, 0x0F, 0x78, 0xC0, 0x10, 0x08, 0x66, 0x48, 0x0F, 0x7E, 0xC0,
                         0xC3});
  CHECK(f(0x1122334455667788ull, 0) == 0x6677ull);
}

TEST_CASE(cpu_emulation_movntss_and_movntsd) {
  am::rt::install_cpu_emulation();
  // movd xmm0, edx ; movntss [rcx], xmm0 ; ret
  const Fn ss = assemble({0x66, 0x0F, 0x6E, 0xC2, 0xF3, 0x0F, 0x2B, 0x01, 0xC3});
  // movq xmm0, rdx ; movntsd [rcx+8], xmm0 ; ret
  const Fn sd = assemble({0x66, 0x48, 0x0F, 0x6E, 0xC2, 0xF2, 0x0F, 0x2B, 0x41, 0x08, 0xC3});
  alignas(16) uint64_t buf[4] = {~0ull, ~0ull, ~0ull, ~0ull};
  ss(reinterpret_cast<uint64_t>(&buf[0]), 0x12345678u);
  CHECK(buf[0] == 0xFFFFFFFF12345678ull);  // 4 bytes stored, the rest untouched
  sd(reinterpret_cast<uint64_t>(&buf[0]), 0x0102030405060708ull);
  CHECK(buf[1] == 0x0102030405060708ull);
  CHECK(buf[2] == ~0ull);
}

TEST_CASE(cpu_emulation_clzero_clears_one_aligned_line) {
  am::rt::install_cpu_emulation();
  // mov rax, rcx ; clzero ; ret
  const Fn f = assemble({0x48, 0x89, 0xC8, 0x0F, 0x01, 0xFC, 0xC3});
  alignas(64) uint8_t buf[192];
  std::memset(buf, 0xAA, sizeof buf);
  f(reinterpret_cast<uint64_t>(buf + 64 + 17), 0);  // an address inside the second line
  bool first = true, second = true, third = true;
  for (int i = 0; i < 64; ++i) {
    first = first && buf[i] == 0xAA;
    second = second && buf[64 + i] == 0;
    third = third && buf[128 + i] == 0xAA;
  }
  CHECK(first);
  CHECK(second);
  CHECK(third);
}

TEST_CASE(cpu_emulation_clwb_monitorx_rdpid_rdpru_continue) {
  am::rt::install_cpu_emulation();
  // clwb [rcx] ; mov rax, rcx ; xor ecx, ecx ; xor edx, edx ; monitorx ; mov eax, 7 ; ret
  const Fn hints = assemble({0x66, 0x0F, 0xAE, 0x31, 0x48, 0x89, 0xC8, 0x31, 0xC9, 0x31, 0xD2, 0x0F, 0x01, 0xFA, 0xB8,
                             0x07, 0x00, 0x00, 0x00, 0xC3});
  uint64_t cell = 0;
  CHECK(hints(reinterpret_cast<uint64_t>(&cell), 0) == 7);
  // rdpid rax ; ret
  const Fn rdpid = assemble({0xF3, 0x0F, 0xC7, 0xF8, 0xC3});
  CHECK(rdpid(0, 0) < 4096);
  // mov ecx, 1 ; rdpru ; shl rdx, 32 ; or rax, rdx ; ret  (APERF: rises)
  const Fn rdpru = assemble({0xB9, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x01, 0xFD, 0x48, 0xC1, 0xE2, 0x20, 0x48, 0x09, 0xD0,
                             0xC3});
  const uint64_t a = rdpru(0, 0);
  const uint64_t b = rdpru(0, 0);
  CHECK(b >= a);
}
