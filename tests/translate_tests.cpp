// The shader translator, checked on the host GPU: small console programs (assembled with LLVM's
// AMDGPU assembler; the source is beside each word) are translated, compiled with DXC, run,
// and every register compared with what the ISA says. Skipped without a D3D12 GPU of shader
// model 6.6.

#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu_runner.h"
#include "host/dxc_compile.h"
#include "shader/translate.h"
#include "test.h"

namespace {

using namespace am::shader;

struct Result {
  bool ran = false;
  uint32_t nv = 0;
  std::vector<uint32_t> dump, memory;
  uint32_t s(int i) const { return dump[i]; }
  bool scc() const { return dump[128] != 0; }
  uint32_t v(int lane, int reg) const { return dump[kDumpLanes / 4 + lane * nv + reg]; }
};

// One wave64 group of 64 threads (rsrc2: 2 user SGPRs, group id x), or as given.
Result run(const std::vector<uint32_t>& code, std::vector<uint32_t> user = {}, std::vector<uint32_t> memory = {},
           uint64_t base = 0, uint32_t rsrc2 = (2u << 1) | (1u << 7)) {
  Result r;
  TranslateInput in;
  in.code = code;
  in.stage = HwStage::Cs;
  in.wave32 = false;
  in.vgprs = 12;
  in.cs_rsrc2 = rsrc2;
  in.cs_threads[0] = 64;
  const Translation t = translate(in, Binding::Test);
  CHECK(t.ok);
  if (!t.ok) {
    std::printf("    not translated: %s\n", t.failure.c_str());
    return r;
  }
  std::string why;
  if (!am::host::dxc_available(&why)) {
    std::printf("    skipped: %s\n", why.c_str());
    return r;
  }
  const am::host::CompileResult c = am::host::compile_hlsl(t.hlsl, t.entry, t.profile);
  CHECK(c.ok);
  if (!c.ok) {
    std::printf("    DXC: %s\n", c.messages.substr(0, 2000).c_str());
    return r;
  }
  std::vector<uint32_t> args(36, 0);
  for (size_t i = 0; i < user.size() && i < 32; ++i) args[i] = user[i];
  args[32] = static_cast<uint32_t>(base);
  args[33] = static_cast<uint32_t>(base >> 32);
  r.nv = t.vgpr_count;
  const am::test::GpuRun g = am::test::run_compute(c.dxil, 1, args, memory, kDumpLanes / 4 + 64 * r.nv);
  if (!g.ran) {
    std::printf("    skipped: %s\n", g.why.c_str());
    return r;
  }
  r.ran = true;
  r.dump = g.dump;
  r.memory = g.memory;
  return r;
}

float f(uint32_t u) {
  float x;
  std::memcpy(&x, &u, 4);
  return x;
}

}  // namespace

TEST_CASE(translate_scalar_alu_and_loops) {
  const Result r = run({
      0xBE800385,              // s_mov_b32 s0, 5
      0x80018700,              // s_add_u32 s1, s0, 7
      0xBF068C01,              // s_cmp_eq_u32 s1, 12
      0x850285FF, 0x00012345,  // s_cselect_b32 s2, 0x12345, 5
      0xBE830380,              // s_mov_b32 s3, 0
      0x80038103,              // s_add_u32 s3, s3, 1      (loop)
      0xBF0A8A03,              // s_cmp_lt_u32 s3, 10
      0xBF85FFFD,              // s_cbranch_scc1 -3
      0x8F048403,              // s_lshl_b32 s4, s3, 4
      0xBE8603FF, 0x00080004,  // s_mov_b32 s6, 0x80004
      0x938506FF, 0xABCDEF12,  // s_bfe_u32 s5, 0xabcdef12, s6
      0x810781FF, 0x7FFFFFFF,  // s_add_i32 s7, 0x7fffffff, 1
      0xBF810000,              // s_endpgm
  });
  if (!r.ran) return;
  CHECK(r.s(0) == 5 && r.s(1) == 12 && r.s(2) == 0x12345);
  CHECK(r.s(3) == 10);  // the loop ran ten times
  CHECK(r.s(4) == 160);
  CHECK(r.s(5) == 0xF1);  // bits 4..11
  CHECK(r.s(7) == 0x80000000u && r.scc());  // signed overflow sets SCC
}

TEST_CASE(translate_vector_alu_and_exec) {
  const Result r = run({
      0x7D88008A,              // v_cmp_gt_u32 vcc, 10, v0        (lanes 0..9)
      0xBE84246A,              // s_and_saveexec_b64 s[4:5], vcc
      0x7E0402AA,              // v_mov_b32 v2, 42                (lanes 0..9 only)
      0xBEFE0404,              // s_mov_b64 exec, s[4:5]
      0x4A060100,              // v_add_nc_u32 v3, v0, v0
      0xD5010004, 0x01A91287,  // v_cndmask_b32_e64 v4, 7, 9, vcc
      0x7E0A0D00,              // v_cvt_f32_u32 v5, v0
      0x0E0C0E80,              // v_mul_legacy_f32 v6, 0, v7     (v7 = 0 here)
      0x100E0AF0,              // v_mul_f32 v7, 0.5, v5
      0xD54B0008, 0x8415E905,  // v_fma_f32 v8, v5, 2.0, -v5
      0xD7650009, 0x020100C1,  // v_mbcnt_lo_u32_b32 v9, -1, 0
      0xD7660009, 0x020212C1,  // v_mbcnt_hi_u32_b32 v9, -1, v9
      0x7E0C0503,              // v_readfirstlane_b32 s6, v3
      0xD548000A, 0x02090300,  // v_bfe_u32 v10, v0, 1, 2
      0xD4D1007E, 0x02012900,  // v_cmpx_lt_u32_e64 v0, 20         (exec: lanes 0..19)
      0x7E160281,              // v_mov_b32 v11, 1
      0xBEFE0404,              // s_mov_b64 exec, s[4:5]
      0xBF810000,              // s_endpgm
  });
  if (!r.ran) return;
  CHECK(r.s(106) == 0x3FF && r.s(107) == 0);  // VCC
  CHECK(r.s(4) == 0xFFFFFFFFu && r.s(5) == 0xFFFFFFFFu && r.s(126) == 0xFFFFFFFFu && r.s(127) == 0xFFFFFFFFu);
  CHECK(r.s(6) == 0);
  bool lanes_ok = true;
  for (int l = 0; l < 64; ++l) {
    lanes_ok = lanes_ok && r.v(l, 0) == static_cast<uint32_t>(l);
    lanes_ok = lanes_ok && r.v(l, 2) == (l < 10 ? 42u : 0u);
    lanes_ok = lanes_ok && r.v(l, 3) == static_cast<uint32_t>(2 * l);
    lanes_ok = lanes_ok && r.v(l, 4) == (l < 10 ? 9u : 7u);
    lanes_ok = lanes_ok && f(r.v(l, 5)) == static_cast<float>(l);
    lanes_ok = lanes_ok && r.v(l, 6) == 0;
    lanes_ok = lanes_ok && f(r.v(l, 7)) == l * 0.5f;
    lanes_ok = lanes_ok && f(r.v(l, 8)) == static_cast<float>(l);
    lanes_ok = lanes_ok && r.v(l, 9) == static_cast<uint32_t>(l);
    lanes_ok = lanes_ok && r.v(l, 10) == ((static_cast<uint32_t>(l) >> 1) & 3);
    lanes_ok = lanes_ok && r.v(l, 11) == (l < 20 ? 1u : 0u);
    if (!lanes_ok) {
      std::printf("    lane %d differs: v2 %u v3 %u v4 %u v6 %08x v9 %u v10 %u v11 %u\n", l, r.v(l, 2), r.v(l, 3),
                  r.v(l, 4), r.v(l, 6), r.v(l, 9), r.v(l, 10), r.v(l, 11));
      break;
    }
  }
  CHECK(lanes_ok);
}

TEST_CASE(translate_scalar_vector_and_buffer_memory) {
  // Memory at a 64-bit GPU address: a value, a V# for the buffer at +0x100, the buffer itself.
  const uint64_t base = 0x123400000ull;
  std::vector<uint32_t> memory(1024, 0);
  memory[0] = 1000;
  const uint64_t buf = base + 0x100;
  memory[2] = static_cast<uint32_t>(buf);
  memory[3] = static_cast<uint32_t>(buf >> 32) & 0xFFFF;  // stride 0
  memory[4] = 256;                                         // bytes
  memory[5] = 0;
  for (int i = 0; i < 64; ++i) memory[0x40 + i] = 0x5000 + i;
  const Result r = run(
      {
          0xF4040080, 0xFA000000,  // s_load_dwordx2 s[2:3], s[0:1], 0x0
          0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
          0x34020082,              // v_lshlrev_b32 v1, 2, v0
          0x4A040002,              // v_add_nc_u32 v2, s2, v0
          0xDC708400, 0x00000201,  // global_store_dword v1, v2, s[0:1] offset:1024
          0xF4080200, 0xFA000008,  // s_load_dwordx4 s[8:11], s[0:1], 0x8
          0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
          0xE0301000, 0x80020301,  // buffer_load_dword v3, v1, s[8:11], 0 offen
          0xBF8C3F70,              // s_waitcnt vmcnt(0)
          0xBF810000,              // s_endpgm
      },
      {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)}, memory, base);
  if (!r.ran) return;
  CHECK(r.s(2) == 1000 && r.s(8) == static_cast<uint32_t>(buf) && r.s(10) == 256);
  bool ok = true;
  for (int l = 0; l < 64; ++l) {
    ok = ok && r.memory[256 + l] == 1000u + l;    // the global store
    ok = ok && r.v(l, 3) == 0x5000u + l;         // the buffer load
  }
  CHECK(ok);
}
