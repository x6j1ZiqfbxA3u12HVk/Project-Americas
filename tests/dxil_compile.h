#pragma once

// The tests' way through our own DXIL: HLSL (the translator's subset) compiled by
// shader/dxil/hlsl.h into a module, finished, written as bitcode and made a signed container by
// DXC's assembler and validator. When the validator computes other shader flags than ours, its
// are taken (and said: Module::finish should learn them).

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "host/dxc_compile.h"
#include "shader/dxil/dxil.h"
#include "shader/dxil/hlsl.h"

namespace am::test {

inline host::CompileResult assemble_module(shader::dxil::Module& m) {
  host::CompileResult r;
  std::string error;
  if (!m.finish(error)) {
    r.messages = error;
    return r;
  }
  r = host::assemble_dxil(shader::dxil::write_bitcode(m));
  uint64_t flags = 0;
  if (!r.ok && shader::dxil::validator_flags(r.messages, flags)) {
    std::printf("    the validator wants flags 0x%llx (ours 0x%llx)\n", static_cast<unsigned long long>(flags),
                static_cast<unsigned long long>(m.shader_flags()));
    m.set_flags(flags);
    r = host::assemble_dxil(shader::dxil::write_bitcode(m));
  }
  return r;
}

// AM_DXIL_DUMP=<folder>: a program our compiler or the validator refuses is written there
// (ours-<n>.hlsl), and the message names it.
inline void dump_refused(std::string_view hlsl, host::CompileResult& r) {
  char dir[512] = {};
  if (!GetEnvironmentVariableA("AM_DXIL_DUMP", dir, sizeof dir) || !dir[0]) return;
  static int n = 0;
  const std::string path = std::string(dir) + "\\ours-" + std::to_string(++n) + ".hlsl";
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
  std::fwrite(hlsl.data(), 1, hlsl.size(), f);
  std::fclose(f);
  r.messages = "(" + path + ") " + r.messages;
}

inline host::CompileResult compile_ours(std::string_view hlsl, std::string_view entry) {
  shader::dxil::Module m;
  host::CompileResult r;
  if (!shader::dxil::compile_hlsl(hlsl, entry, m, r.messages)) {
    r.messages = "our compiler: " + r.messages;
    dump_refused(hlsl, r);
    return r;
  }
  r = assemble_module(m);
  if (!r.ok) dump_refused(hlsl, r);
  return r;
}

// AM_DXIL=1: the translator's tests take the goto form through our compiler instead of DXC.
inline bool use_ours() {
  char v[8] = {};
  return GetEnvironmentVariableA("AM_DXIL", v, sizeof v) && v[0] == '1';
}

}  // namespace am::test
