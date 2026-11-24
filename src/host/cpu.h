#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace am::host {

// The CPUID results the feature checks read. read_cpuid() fills it from the running CPU;
// tests fill it by hand.
struct CpuidSnapshot {
  std::string vendor;  // "GenuineIntel", "AuthenticAMD"
  std::string brand;
  uint32_t max_leaf = 0;
  uint32_t max_ext_leaf = 0;
  uint32_t l1_ecx = 0, l1_edx = 0;                // leaf 1
  uint32_t l7_ebx = 0, l7_ecx = 0, l7_edx = 0;    // leaf 7, sub-leaf 0
  uint32_t ld1_eax = 0;                           // leaf 0xD, sub-leaf 1
  uint32_t e1_ecx = 0, e1_edx = 0;                // leaf 0x80000001
  uint32_t e8_ebx = 0;                            // leaf 0x80000008
  uint64_t xcr0 = 0;                              // meaningful when the OS uses XSAVE
};

CpuidSnapshot read_cpuid();

// Instruction-set features of the console's CPU cores (AMD Zen 2) that title code may use
// without asking first: the console's hardware is fixed, so its code need not check.
enum class CpuFeature : uint8_t {
  SSE3, SSSE3, SSE41, SSE42, SSE4A, POPCNT, LZCNT, CX16,
  AVX, AVX2, FMA3, F16C, BMI1, BMI2, ADX, MOVBE,
  AES, PCLMULQDQ, SHA, RDRAND, RDSEED,
  XSAVE, XSAVEOPT, XSAVEC, XSAVES,
  CLFLUSHOPT, CLWB, CLZERO, PREFETCHW,
  RDTSCP, RDPID, RDPRU, MONITORX,
};

struct CpuFeatureInfo {
  CpuFeature feature;
  const char* name;
  const char* note;  // what the instructions do; how a host without them is handled
};

std::span<const CpuFeatureInfo> console_cpu_features();
const CpuFeatureInfo& feature_info(CpuFeature f);
bool has_feature(const CpuidSnapshot& cpu, CpuFeature f);
// The console features this CPU lacks; title code that uses them must be patched (see the plan).
std::vector<CpuFeature> missing_console_features(const CpuidSnapshot& cpu);

}  // namespace am::host
