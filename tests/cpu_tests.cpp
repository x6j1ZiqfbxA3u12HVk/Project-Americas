#include <algorithm>

#include "host/cpu.h"
#include "test.h"

using namespace am::host;

namespace {

// CPUID bits of a Zen 2 core with an OS that saves AVX state.
CpuidSnapshot zen2() {
  CpuidSnapshot s;
  s.vendor = "AuthenticAMD";
  s.max_leaf = 0x10;
  s.max_ext_leaf = 0x80000020u;
  s.l1_ecx = (1u << 0) | (1u << 1) | (1u << 9) | (1u << 12) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 22) |
             (1u << 23) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 30);
  s.l7_ebx = (1u << 3) | (1u << 5) | (1u << 8) | (1u << 18) | (1u << 19) | (1u << 23) | (1u << 24) | (1u << 29);
  s.l7_ecx = 1u << 22;
  s.ld1_eax = (1u << 0) | (1u << 1) | (1u << 3);
  s.e1_ecx = (1u << 5) | (1u << 6) | (1u << 8) | (1u << 29);
  s.e1_edx = 1u << 27;
  s.e8_ebx = (1u << 0) | (1u << 4);
  s.xcr0 = 0x7;
  return s;
}

// The same CPU without what an Intel Kaby Lake lacks.
CpuidSnapshot kaby_lake() {
  CpuidSnapshot s = zen2();
  s.vendor = "GenuineIntel";
  s.l7_ebx &= ~((1u << 24) | (1u << 29));  // CLWB, SHA
  s.l7_ecx &= ~(1u << 22);                 // RDPID
  s.e1_ecx &= ~((1u << 6) | (1u << 29));   // SSE4a, MONITORX
  s.e8_ebx = 0;                            // CLZERO, RDPRU
  return s;
}

bool contains(const std::vector<CpuFeature>& v, CpuFeature f) { return std::find(v.begin(), v.end(), f) != v.end(); }

}  // namespace

TEST_CASE(zen2_has_every_console_feature) { CHECK(missing_console_features(zen2()).empty()); }

TEST_CASE(kaby_lake_lacks_the_amd_and_ice_lake_features) {
  const auto missing = missing_console_features(kaby_lake());
  CHECK(missing.size() == 7);
  for (CpuFeature f : {CpuFeature::CLWB, CpuFeature::SHA, CpuFeature::RDPID, CpuFeature::SSE4A, CpuFeature::MONITORX,
                       CpuFeature::CLZERO, CpuFeature::RDPRU}) {
    CHECK(contains(missing, f));
  }
}

TEST_CASE(avx_needs_os_state_support) {
  CpuidSnapshot s = zen2();
  s.xcr0 = 0x1;  // the OS does not save YMM registers
  const auto missing = missing_console_features(s);
  CHECK(contains(missing, CpuFeature::AVX));
  CHECK(contains(missing, CpuFeature::AVX2));
  CHECK(contains(missing, CpuFeature::FMA3));
  CHECK(contains(missing, CpuFeature::F16C));
  CHECK(!contains(missing, CpuFeature::SSE42));
}

TEST_CASE(this_cpu_reads) {
  const CpuidSnapshot s = read_cpuid();
  CHECK(!s.vendor.empty());
  CHECK(has_feature(s, CpuFeature::SSE42));  // every x64 Windows 11 CPU has it
}
