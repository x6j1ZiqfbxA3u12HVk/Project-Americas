#pragma once

// A small D3D12 compute harness for the translator's tests: runs one compiled program with the
// tests' binding model (constant buffer b0, memory u0, register dump u1) on the host GPU.

#include <cstdint>
#include <string>
#include <vector>

namespace am::test {

struct GpuRun {
  bool ran = false;
  std::string why;  // when it did not: no GPU, no SM 6.6, a failure
  std::vector<uint32_t> memory;
  std::vector<uint32_t> dump;
};

// `args` is the constant buffer (32 user SGPRs, then the memory's GPU base address).
GpuRun run_compute(const std::vector<uint8_t>& dxil, uint32_t groups, const std::vector<uint32_t>& args,
                   std::vector<uint32_t> memory, size_t dump_dwords);

}  // namespace am::test
