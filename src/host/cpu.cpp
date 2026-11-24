#include "host/cpu.h"

#include <immintrin.h>
#include <intrin.h>

#include <cstring>

namespace am::host {
namespace {

constexpr CpuFeatureInfo kFeatures[] = {
    {CpuFeature::SSE3, "SSE3", ""},
    {CpuFeature::SSSE3, "SSSE3", ""},
    {CpuFeature::SSE41, "SSE4.1", ""},
    {CpuFeature::SSE42, "SSE4.2", ""},
    {CpuFeature::SSE4A, "SSE4a", "EXTRQ/INSERTQ/MOVNTSS/MOVNTSD; AMD only"},
    {CpuFeature::POPCNT, "POPCNT", ""},
    {CpuFeature::LZCNT, "LZCNT", ""},
    {CpuFeature::CX16, "CMPXCHG16B", ""},
    {CpuFeature::AVX, "AVX", ""},
    {CpuFeature::AVX2, "AVX2", ""},
    {CpuFeature::FMA3, "FMA3", ""},
    {CpuFeature::F16C, "F16C", ""},
    {CpuFeature::BMI1, "BMI1", ""},
    {CpuFeature::BMI2, "BMI2", ""},
    {CpuFeature::ADX, "ADX", ""},
    {CpuFeature::MOVBE, "MOVBE", ""},
    {CpuFeature::AES, "AES-NI", ""},
    {CpuFeature::PCLMULQDQ, "PCLMULQDQ", ""},
    {CpuFeature::SHA, "SHA", "SHA-1/SHA-256 rounds; Intel from Ice Lake"},
    {CpuFeature::RDRAND, "RDRAND", ""},
    {CpuFeature::RDSEED, "RDSEED", ""},
    {CpuFeature::XSAVE, "XSAVE", ""},
    {CpuFeature::XSAVEOPT, "XSAVEOPT", ""},
    {CpuFeature::XSAVEC, "XSAVEC", ""},
    {CpuFeature::XSAVES, "XSAVES", "kernel mode only; listed for completeness"},
    {CpuFeature::CLFLUSHOPT, "CLFLUSHOPT", ""},
    {CpuFeature::CLWB, "CLWB", "cache-line write-back; a no-op on coherent PC memory; Intel from Ice Lake"},
    {CpuFeature::CLZERO, "CLZERO", "zeroes a 64-byte cache line; AMD only"},
    {CpuFeature::PREFETCHW, "PREFETCHW", ""},
    {CpuFeature::RDTSCP, "RDTSCP", ""},
    {CpuFeature::RDPID, "RDPID", "reads the processor id; Intel from Ice Lake"},
    {CpuFeature::RDPRU, "RDPRU", "reads performance registers; AMD only"},
    {CpuFeature::MONITORX, "MONITORX/MWAITX", "user-mode wait on a cache line; AMD only"},
};

bool bit(uint32_t reg, int n) { return ((reg >> n) & 1u) != 0; }

}  // namespace

CpuidSnapshot read_cpuid() {
  CpuidSnapshot s;
  int r[4] = {};
  __cpuid(r, 0);
  s.max_leaf = static_cast<uint32_t>(r[0]);
  char vendor[13] = {};
  std::memcpy(vendor + 0, &r[1], 4);
  std::memcpy(vendor + 4, &r[3], 4);
  std::memcpy(vendor + 8, &r[2], 4);
  s.vendor = vendor;

  if (s.max_leaf >= 1) {
    __cpuid(r, 1);
    s.l1_ecx = static_cast<uint32_t>(r[2]);
    s.l1_edx = static_cast<uint32_t>(r[3]);
  }
  if (s.max_leaf >= 7) {
    __cpuidex(r, 7, 0);
    s.l7_ebx = static_cast<uint32_t>(r[1]);
    s.l7_ecx = static_cast<uint32_t>(r[2]);
    s.l7_edx = static_cast<uint32_t>(r[3]);
  }
  if (s.max_leaf >= 0xD) {
    __cpuidex(r, 0xD, 1);
    s.ld1_eax = static_cast<uint32_t>(r[0]);
  }
  if (bit(s.l1_ecx, 27)) s.xcr0 = _xgetbv(0);  // OSXSAVE

  __cpuid(r, static_cast<int>(0x80000000u));
  s.max_ext_leaf = static_cast<uint32_t>(r[0]);
  if (s.max_ext_leaf >= 0x80000001u) {
    __cpuid(r, static_cast<int>(0x80000001u));
    s.e1_ecx = static_cast<uint32_t>(r[2]);
    s.e1_edx = static_cast<uint32_t>(r[3]);
  }
  if (s.max_ext_leaf >= 0x80000004u) {
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
      __cpuid(r, static_cast<int>(0x80000002u + i));
      std::memcpy(brand + i * 16, r, 16);
    }
    s.brand = brand;
    while (!s.brand.empty() && s.brand.back() == ' ') s.brand.pop_back();
    while (!s.brand.empty() && s.brand.front() == ' ') s.brand.erase(0, 1);
  }
  if (s.max_ext_leaf >= 0x80000008u) {
    __cpuid(r, static_cast<int>(0x80000008u));
    s.e8_ebx = static_cast<uint32_t>(r[1]);
  }
  return s;
}

std::span<const CpuFeatureInfo> console_cpu_features() { return kFeatures; }

const CpuFeatureInfo& feature_info(CpuFeature f) {
  for (const CpuFeatureInfo& info : kFeatures) {
    if (info.feature == f) return info;
  }
  return kFeatures[0];
}

bool has_feature(const CpuidSnapshot& c, CpuFeature f) {
  // AVX-class instructions also need the OS to save the YMM state (XCR0 bits 1 and 2).
  const bool avx_state = bit(c.l1_ecx, 27) && (c.xcr0 & 0x6) == 0x6;
  const bool l7 = c.max_leaf >= 7;
  const bool ld = c.max_leaf >= 0xD;
  const bool e1 = c.max_ext_leaf >= 0x80000001u;
  const bool e8 = c.max_ext_leaf >= 0x80000008u;
  switch (f) {
    case CpuFeature::SSE3: return bit(c.l1_ecx, 0);
    case CpuFeature::PCLMULQDQ: return bit(c.l1_ecx, 1);
    case CpuFeature::SSSE3: return bit(c.l1_ecx, 9);
    case CpuFeature::FMA3: return bit(c.l1_ecx, 12) && avx_state;
    case CpuFeature::CX16: return bit(c.l1_ecx, 13);
    case CpuFeature::SSE41: return bit(c.l1_ecx, 19);
    case CpuFeature::SSE42: return bit(c.l1_ecx, 20);
    case CpuFeature::MOVBE: return bit(c.l1_ecx, 22);
    case CpuFeature::POPCNT: return bit(c.l1_ecx, 23);
    case CpuFeature::AES: return bit(c.l1_ecx, 25);
    case CpuFeature::XSAVE: return bit(c.l1_ecx, 26);
    case CpuFeature::AVX: return bit(c.l1_ecx, 28) && avx_state;
    case CpuFeature::F16C: return bit(c.l1_ecx, 29) && avx_state;
    case CpuFeature::RDRAND: return bit(c.l1_ecx, 30);
    case CpuFeature::BMI1: return l7 && bit(c.l7_ebx, 3);
    case CpuFeature::AVX2: return l7 && bit(c.l7_ebx, 5) && avx_state;
    case CpuFeature::BMI2: return l7 && bit(c.l7_ebx, 8);
    case CpuFeature::RDSEED: return l7 && bit(c.l7_ebx, 18);
    case CpuFeature::ADX: return l7 && bit(c.l7_ebx, 19);
    case CpuFeature::CLFLUSHOPT: return l7 && bit(c.l7_ebx, 23);
    case CpuFeature::CLWB: return l7 && bit(c.l7_ebx, 24);
    case CpuFeature::SHA: return l7 && bit(c.l7_ebx, 29);
    case CpuFeature::RDPID: return l7 && bit(c.l7_ecx, 22);
    case CpuFeature::XSAVEOPT: return ld && bit(c.ld1_eax, 0);
    case CpuFeature::XSAVEC: return ld && bit(c.ld1_eax, 1);
    case CpuFeature::XSAVES: return ld && bit(c.ld1_eax, 3);
    case CpuFeature::LZCNT: return e1 && bit(c.e1_ecx, 5);
    case CpuFeature::SSE4A: return e1 && bit(c.e1_ecx, 6);
    case CpuFeature::PREFETCHW: return e1 && bit(c.e1_ecx, 8);
    case CpuFeature::MONITORX: return e1 && bit(c.e1_ecx, 29);
    case CpuFeature::RDTSCP: return e1 && bit(c.e1_edx, 27);
    case CpuFeature::CLZERO: return e8 && bit(c.e8_ebx, 0);
    case CpuFeature::RDPRU: return e8 && bit(c.e8_ebx, 4);
  }
  return false;
}

std::vector<CpuFeature> missing_console_features(const CpuidSnapshot& cpu) {
  std::vector<CpuFeature> missing;
  for (const CpuFeatureInfo& info : kFeatures) {
    if (!has_feature(cpu, info.feature)) missing.push_back(info.feature);
  }
  return missing;
}

}  // namespace am::host
