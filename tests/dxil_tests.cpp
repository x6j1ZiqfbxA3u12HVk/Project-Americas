// Our own DXIL: modules written by the translator (shader/dxil), made into signed containers by
// DXC's assembler and validator, run on the host GPU.

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

#include "gpu_runner.h"
#include "host/dxc_compile.h"
#include "shader/container.h"
#include "shader/dxil/dxil.h"
#include "test.h"

namespace {

// The LLVM bitcode inside a DXIL container's DXIL part (after its 24-byte program header).
std::vector<uint8_t> bitcode_of(const std::vector<uint8_t>& container) {
  std::string error;
  const auto c = am::shader::parse_container(container, error);
  const am::shader::Part* part = c ? c->find("DXIL") : nullptr;
  if (!part || part->data.size() < 24) return {};
  uint32_t offset = 0, size = 0;
  std::memcpy(&offset, part->data.data() + 16, 4);  // DxilBitcodeHeader: magic, version, offset, size
  std::memcpy(&size, part->data.data() + 20, 4);
  const size_t at = 8 + offset;
  if (at + size > part->data.size()) return {};
  return std::vector<uint8_t>(part->data.begin() + static_cast<std::ptrdiff_t>(at), part->data.begin() + static_cast<std::ptrdiff_t>(at + size));
}

// Our module into a signed container. When the validator computes other shader flags than ours,
// takes its flags and says so (the flags are worked out in Module::finish).
am::host::CompileResult assemble(am::shader::dxil::Module& m) {
  std::string error;
  if (!m.finish(error)) {
    am::host::CompileResult r;
    r.messages = error;
    return r;
  }
  const std::vector<uint8_t> bitcode = am::shader::dxil::write_bitcode(m);
  char dump[512];
  if (GetEnvironmentVariableA("AM_DXIL_DUMP", dump, sizeof dump)) {  // our bitcode, for llvm-bcanalyzer
    std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(bitcode.data()), static_cast<std::streamsize>(bitcode.size()));
  }
  am::host::CompileResult r = am::host::assemble_dxil(bitcode);
  uint64_t flags = 0;
  if (!r.ok && am::shader::dxil::validator_flags(r.messages, flags)) {
    std::printf("    the validator wants flags 0x%llx (ours 0x%llx)\n", static_cast<unsigned long long>(flags),
                static_cast<unsigned long long>(m.shader_flags()));
    m.set_flags(flags);
    r = am::host::assemble_dxil(am::shader::dxil::write_bitcode(m));
  }
  return r;
}

uint32_t collatz_steps(uint32_t x) {
  uint32_t n = 0;
  while (x != 1u) {
    x = (x & 1u) ? 3u * x + 1u : x >> 1;
    ++n;
  }
  return n;
}

}  // namespace

// DXC's assembler makes a signed container from a module: with LLVM IR text (if it parses it)
// and with bitcode.
TEST_CASE(dxil_assembler_makes_containers) {
  using namespace am;
  std::string why;
  if (!host::dxc_available(&why)) return;
  const host::CompileResult c = host::compile_hlsl(
      "RWByteAddressBuffer o : register(u0);\n[numthreads(64, 1, 1)] void main(uint i : SV_GroupIndex) { o.Store(i * 4, i * 3u); }\n",
      "main", "cs_6_6");
  CHECK(c.ok);
  if (!c.ok) return;
  const std::vector<uint8_t> bitcode = bitcode_of(c.dxil);
  CHECK(!bitcode.empty());
  const host::CompileResult from_bitcode = host::assemble_dxil(bitcode);
  std::printf("    from bitcode: %s (%zu bytes) %s\n", from_bitcode.ok ? "ok" : "failed", from_bitcode.dxil.size(),
              from_bitcode.messages.substr(0, 300).c_str());
  CHECK(from_bitcode.ok);
  const char* text =
      "target datalayout = \"e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64\"\n"
      "target triple = \"dxil-ms-dx\"\n";
  const host::CompileResult from_text = host::assemble_dxil(std::vector<uint8_t>(text, text + std::strlen(text)));
  std::printf("    from text: %s %s\n", from_text.ok ? "ok" : "failed", from_text.messages.substr(0, 300).c_str());
}

// A program built in our own DXIL, with branches HLSL would have had to nest: a loop whose body
// is an if/else, registers as variables (made SSA by the module), memory through a bound raw
// buffer and a constant buffer. Runs on the GPU and is checked against the CPU.
TEST_CASE(dxil_own_module_runs_on_the_gpu) {
  using namespace am::shader::dxil;
  std::string why;
  if (!am::host::dxc_available(&why)) return;
  Module m;
  m.set_compute(64, 1, 1);
  const uint32_t X = m.variable(m.i32()), S = m.variable(m.i32());
  const Block head = m.block(), body = m.block(), odd = m.block(), even = m.block(), latch = m.block(), done = m.block();
  m.at(0);
  const Value cb = m.bind_cbuffer(0, 0, 80);
  const Value mem = m.bind_raw_buffer(ResourceClass::Uav, 0, 0);
  const Value off = m.shl(m.thread_id(0), m.u32(2));
  const Value x0 = m.extract(m.raw_load(m.i32(), mem, off, 1, 4), 0);
  m.write(X, m.add(x0, m.extract(m.cbuffer_row(cb, m.u32(0)), 1)));
  m.write(S, m.u32(0));
  m.br(head);
  m.at(head);
  m.cond_br(m.cmp(Pred::IEq, m.read(X), m.u32(1)), done, body);
  m.at(body);
  m.cond_br(m.cmp(Pred::INe, m.and_(m.read(X), m.u32(1)), m.u32(0)), odd, even);
  m.at(odd);
  m.write(X, m.add(m.mul(m.read(X), m.u32(3)), m.u32(1)));
  m.br(latch);
  m.at(even);
  m.write(X, m.lshr(m.read(X), m.u32(1)));
  m.br(latch);
  m.at(latch);
  m.write(S, m.add(m.read(S), m.u32(1)));
  m.br(head);
  m.at(done);
  m.raw_store(mem, off, {m.read(S)}, 4);
  m.ret();

  const am::host::CompileResult c = assemble(m);
  std::printf("    our module: %s (%zu bytes of container) %s\n", c.ok ? "ok" : "refused", c.dxil.size(),
              c.messages.substr(0, 600).c_str());
  std::fflush(stdout);
  CHECK(c.ok);
  if (!c.ok) return;
  constexpr uint32_t kThreads = 256, kAdd = 5;
  std::vector<uint32_t> memory(kThreads), args(40, 0);
  for (uint32_t i = 0; i < kThreads; ++i) memory[i] = i + 1;
  args[1] = kAdd;  // row 0, .y
  const am::test::GpuRun g = am::test::run_compute(c.dxil, kThreads / 64, args, memory, 64);
  if (!g.ran) {
    std::printf("    not run: %s\n", g.why.c_str());
    return;
  }
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < kThreads; ++i) wrong += g.memory[i] != collatz_steps(i + 1 + kAdd);
  std::printf("    %u of %u threads wrong (thread 0: %u, wanted %u)\n", wrong, kThreads, g.memory[0], collatz_steps(1 + kAdd));
  CHECK(wrong == 0);
}
