// The shader translator, checked on the host GPU: small console programs (assembled with LLVM's
// AMDGPU assembler; the source is beside each word) are translated, compiled with DXC, run,
// and every register compared with what the ISA says. Skipped without a D3D12 GPU of shader
// model 6.6.

#include <cmath>
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

namespace {
// The half-precision bits of a small whole number (exact below 2048).
uint32_t half_of(uint32_t n) {
  if (n == 0) return 0;
  uint32_t e = 0;
  while ((n >> (e + 1)) != 0) ++e;
  return (e + 15) << 10 | ((n << (10 - e)) & 0x3FF);
}
bool near(uint32_t got, float want) { return std::fabs(f(got) - want) <= 1e-6f; }
}  // namespace

TEST_CASE(translate_typed_buffers) {
  // V#s (raw, 4 KiB each): A and B over 8_8_8_8_UNORM elements at +0x400 (B swizzles to Z, Y, X, 1);
  // C at +0x800 for the typed loads, which give their own formats; D at +0x1400 for byte stores;
  // E at +0x1800 over 16_16_16_16_FLOAT.
  const uint64_t base = 0x200000000ull;
  std::vector<uint32_t> memory(2048, 0);
  const auto vsharp = [&](int at, uint32_t offset, uint32_t w3) {
    const uint64_t a = base + offset;
    memory[at] = static_cast<uint32_t>(a);
    memory[at + 1] = static_cast<uint32_t>(a >> 32) & 0xFFFF;
    memory[at + 2] = 0x1000;
    memory[at + 3] = w3;
  };
  const uint32_t identity = 4 | 5 << 3 | 6 << 6 | 7 << 9;
  vsharp(0, 0x400, identity | 56u << 12);
  vsharp(4, 0x400, (6 | 5 << 3 | 4 << 6 | 1 << 9) | 56u << 12);
  vsharp(8, 0x800, identity);
  vsharp(12, 0x1400, identity);
  vsharp(16, 0x1800, identity | 71u << 12);
  for (uint32_t l = 0; l < 64; ++l) {
    memory[0x100 + l] = l | (2 * l) << 8 | 255u << 16;                     // 8_8_8_8 (l, 2l, 255, 0)
    memory[0x200 + l] = half_of(l) | 0xB800u << 16;                        // 16_16_FLOAT (l, -0.5)
    memory[0x300 + l] = (16 * l) | 1023u << 10 | 3u << 30;                  // 2_10_10_10 (16l, max, 0, max)
    memory[0x400 + l] = 0x3C0 | 0x400u << 11 | 0x1C0u << 22;               // 10_11_11_FLOAT (1, 2, 0.5)
  }
  const Result r = run(
      {
          0xF4080100, 0xFA000000,  // s_load_dwordx4 s[4:7], s[0:1], 0x0
          0xF4080200, 0xFA000010,  // s_load_dwordx4 s[8:11], s[0:1], 0x10
          0xF4080300, 0xFA000020,  // s_load_dwordx4 s[12:15], s[0:1], 0x20
          0xF4080400, 0xFA000030,  // s_load_dwordx4 s[16:19], s[0:1], 0x30
          0xF4080500, 0xFA000040,  // s_load_dwordx4 s[20:23], s[0:1], 0x40
          0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
          0x34020082,              // v_lshlrev_b32 v1, 2, v0
          0x34040083,              // v_lshlrev_b32 v2, 3, v0
          0xE00C1000, 0x80010401,  // buffer_load_format_xyzw v[4:7], v1, s[4:7], 0 offen
          0xE00C1000, 0x80020801,  // buffer_load_format_xyzw v[8:11], v1, s[8:11], 0 offen
          0xE8E91000, 0x80030C01,  // tbuffer_load_format_xy v[12:13], v1, s[12:15], 0 format:[BUF_FMT_16_16_FLOAT] offen
          0xE9931400, 0x80030E01,  // tbuffer_load_format_xyzw v[14:17], v1, s[12:15], 0 format:[BUF_FMT_2_10_10_10_UNORM] offen offset:1024
          0xE9221800, 0x80031201,  // tbuffer_load_format_xyz v[18:20], v1, s[12:15], 0 format:[BUF_FMT_10_11_11_FLOAT] offen offset:2048
          0xBF8C3F70,              // s_waitcnt vmcnt(0)
          0x7E2A0D00,              // v_cvt_f32_u32 v21, v0
          0x102A2AFF, 0x3B808081,  // v_mul_f32 v21, 0x3b808081, v21        (l / 255)
          0xE80C1000, 0x80041500,  // tbuffer_store_format_x v21, v0, s[16:19], 0 format:[BUF_FMT_8_UNORM] offen
          0x7E300D00,              // v_cvt_f32_u32 v24, v0
          0x7E3202F2,              // v_mov_b32 v25, 1.0
          0x7E3402F5,              // v_mov_b32 v26, -2.0
          0x7E3602F0,              // v_mov_b32 v27, 0.5
          0xE01C1000, 0x80051802,  // buffer_store_format_xyzw v[24:27], v2, s[20:23], 0 offen
          0xBF810000,              // s_endpgm
      },
      {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)}, memory, base);
  if (!r.ran) return;
  bool ok = true;
  for (uint32_t l = 0; l < 64 && ok; ++l) {
    const float u = l / 255.0f, u2 = 2 * l / 255.0f;
    ok = near(r.v(l, 4), u) && near(r.v(l, 5), u2) && f(r.v(l, 6)) == 1.0f && r.v(l, 7) == 0;
    ok = ok && f(r.v(l, 8)) == 1.0f && near(r.v(l, 9), u2) && near(r.v(l, 10), u) && f(r.v(l, 11)) == 1.0f;
    ok = ok && f(r.v(l, 12)) == static_cast<float>(l) && f(r.v(l, 13)) == -0.5f;
    ok = ok && near(r.v(l, 14), 16 * l / 1023.0f) && f(r.v(l, 15)) == 1.0f && r.v(l, 16) == 0 && f(r.v(l, 17)) == 1.0f;
    ok = ok && f(r.v(l, 18)) == 1.0f && f(r.v(l, 19)) == 2.0f && f(r.v(l, 20)) == 0.5f;
    ok = ok && ((r.memory[0x500 + l / 4] >> (8 * (l % 4))) & 0xFF) == l;  // bytes from neighbouring lanes
    ok = ok && r.memory[0x600 + 2 * l] == (half_of(l) | 0x3C00u << 16) && r.memory[0x601 + 2 * l] == (0xC000u | 0x3800u << 16);
    if (!ok) {
      std::printf("    lane %u differs: v4..7 %08x %08x %08x %08x  v8..11 %08x %08x %08x %08x  v12,13 %08x %08x\n", l,
                  r.v(l, 4), r.v(l, 5), r.v(l, 6), r.v(l, 7), r.v(l, 8), r.v(l, 9), r.v(l, 10), r.v(l, 11), r.v(l, 12), r.v(l, 13));
      std::printf("      v14..17 %08x %08x %08x %08x  v18..20 %08x %08x %08x  byte %08x  e %08x %08x\n", r.v(l, 14),
                  r.v(l, 15), r.v(l, 16), r.v(l, 17), r.v(l, 18), r.v(l, 19), r.v(l, 20), r.memory[0x500 + l / 4],
                  r.memory[0x600 + 2 * l], r.memory[0x601 + 2 * l]);
    }
  }
  CHECK(ok);
}

TEST_CASE(translate_lane_ops_sdwa_dpp_and_mixed_precision) {
  const Result r = run({
      0x7E0402FA, 0xFF091100,  // v_mov_b32_dpp v2, v0 row_shr:1 bound_ctrl:1
      0x4A0600FA, 0xFF001B00,  // v_add_nc_u32_dpp v3, v0, v0 quad_perm:[3,2,1,0]
      0x7E0802E9, 0x05397700,  // v_mov_b32_dpp v4, v0 dpp8:[7,6,5,4,3,2,1,0]
      0x7E0A02FF, 0x00000063,  // v_mov_b32 v5, 99
      0x7E0A02FA, 0xFF011300,  // v_mov_b32_dpp v5, v0 row_shr:3          (no bound_ctrl: row edges keep 99)
      0x7E0C0287,              // v_mov_b32 v6, 7
      0x7E0C02FA, 0x5F016100,  // v_mov_b32_dpp v6, v0 row_xmask:1 row_mask:0x5
      0x7E1002FF, 0x80C1D2E3,  // v_mov_b32 v8, 0x80c1d2e3
      0x4A0E00F9, 0x06010508,  // v_add_nc_u32_sdwa v7, v8, v0 dst_sel:WORD_1 dst_unused:UNUSED_PAD src0_sel:BYTE_1
      0x7E1202FF, 0x12345678,  // v_mov_b32 v9, 0x12345678
      0x7E1202F9, 0x00031008,  // v_mov_b32_sdwa v9, v8 dst_sel:BYTE_0 dst_unused:UNUSED_PRESERVE src0_sel:BYTE_3
      0x7E1402F9, 0x000A0608,  // v_mov_b32_sdwa v10, sext(v8) src0_sel:BYTE_2
      0x7E160285,              // v_mov_b32 v11, 5
      0x7D8816F9, 0x06009400,  // v_cmp_gt_u32_sdwa s[20:21], v0, v11 src0_sel:BYTE_0
      0xBE9603FF, 0x89ABCDEF,  // s_mov_b32 s22, 0x89abcdef
      0xBE9703FF, 0x01234567,  // s_mov_b32 s23, 0x1234567
      0xD778000C, 0x005C2D00,  // v_permlanex16_b32 v12, v0, s22, s23      (the other row, mirrored)
      0xD54E000D, 0x02221308,  // v_alignbit_b32 v13, v8, v9, 8
      0x7E1C02F2,              // v_mov_b32 v14, 1.0
      0x7E1E02F5,              // v_mov_b32 v15, -2.0
      0x7E2002F0,              // v_mov_b32 v16, 0.5
      0xD5440011, 0x04421F0E,  // v_cubeid_f32 v17, v14, v15, v16
      0xD5450012, 0x04421F0E,  // v_cubesc_f32 v18, v14, v15, v16
      0xD5460013, 0x04421F0E,  // v_cubetc_f32 v19, v14, v15, v16
      0xD5470014, 0x04421F0E,  // v_cubema_f32 v20, v14, v15, v16
      0x7E2A02FF, 0x40004200,  // v_mov_b32 v21, 0x40004200                (halves 2.0 : 3.0)
      0xCC200816, 0x1C3A2B15,  // v_fma_mix_f32 v22, v21, v21, v14 op_sel:[1,0,0] op_sel_hi:[1,1,0]
      0xCC210015, 0x04421F0E,  // v_fma_mixlo_f16 v21, v14, v15, v16
      0xBE980480,              // s_mov_b64 s[24:25], 0
      0xBE981EA8,              // s_bitset1_b64 s[24:25], 40
      0xBE9B1418,              // s_ff1_i32_b64 s27, s[24:25]
      0xBE9C1018,              // s_bcnt1_i32_b64 s28, s[24:25]
      0xBE9D1519,              // s_flbit_i32_b32 s29, s25
      0xBE9E03C1,              // s_mov_b32 s30, -1
      0xBE9E1B83,              // s_bitset0_b32 s30, 3
      0xBF810000,              // s_endpgm
  });
  if (!r.ran) return;
  CHECK(r.s(20) == 0xFFFFFFC0u && r.s(21) == 0xFFFFFFFFu);  // lanes above 5
  CHECK(r.s(24) == 0 && r.s(25) == 0x100 && r.s(27) == 40 && r.s(28) == 1 && r.s(29) == 23 && r.s(30) == 0xFFFFFFF7u);
  bool ok = true;
  for (uint32_t l = 0; l < 64 && ok; ++l) {
    const uint32_t i = l & 15, row = l >> 4;
    const uint32_t want[] = {
        i >= 1 ? l - 1 : 0u,                                   // v2
        ((l & ~3u) | (3 - (l & 3))) + l,                       // v3
        (l & ~7u) | (7 - (l & 7)),                             // v4
        i >= 3 ? l - 3 : 99u,                                  // v5
        row == 0 || row == 2 ? l ^ 1 : 7u,                     // v6
        ((0xD2 + l) & 0xFFFF) << 16,                           // v7
        0x80C1D2E3u, 0x12345680u, 0xFFFFFFC1u, 5u,             // v8..v11
        ((l & ~15u) ^ 16u) | (15 - i),                         // v12
        0xE3123456u,                                           // v13
    };
    for (uint32_t k = 0; k < 12; ++k) {
      if (r.v(l, 2 + k) != want[k]) {
        std::printf("    lane %u: v%u is %08x, not %08x\n", l, 2 + k, r.v(l, 2 + k), want[k]);
        ok = false;
      }
    }
    ok = ok && f(r.v(l, 17)) == 3.0f && f(r.v(l, 18)) == 1.0f && f(r.v(l, 19)) == -0.5f && f(r.v(l, 20)) == -4.0f;
    ok = ok && f(r.v(l, 22)) == 7.0f && r.v(l, 21) == 0x4000BE00u;
    if (!ok) {
      std::printf("    lane %u: cube %08x %08x %08x %08x  mix %08x %08x\n", l, r.v(l, 17), r.v(l, 18), r.v(l, 19), r.v(l, 20),
                  r.v(l, 22), r.v(l, 21));
    }
  }
  CHECK(ok);
}

TEST_CASE(translate_buffer_and_global_atomics) {
  const uint64_t base = 0x300000000ull;
  std::vector<uint32_t> memory(64, 0);
  memory[1] = 1000;
  memory[4] = 0x80000000u;
  memory[0x10] = static_cast<uint32_t>(base);  // a V# over the first 64 bytes
  memory[0x11] = static_cast<uint32_t>(base >> 32);
  memory[0x12] = 64;
  const Result r = run(
      {
          0xF4080200, 0xFA000040,  // s_load_dwordx4 s[8:11], s[0:1], 0x40
          0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
          0x7E020281,              // v_mov_b32 v1, 1
          0xE0C84000, 0x80020100,  // buffer_atomic_add v1, off, s[8:11], 0 glc
          0x7E040300,              // v_mov_b32 v2, v0
          0xE0D80004, 0x80020200,  // buffer_atomic_umin v2, off, s[8:11], 0 offset:4
          0x4A060081,              // v_add_nc_u32 v3, 1, v0
          0x7E080280,              // v_mov_b32 v4, 0
          0xE0C44008, 0x80020300,  // buffer_atomic_cmpswap v[3:4], off, s[8:11], 0 offset:8 glc
          0xD51A0005, 0x02010300,  // v_lshlrev_b32 v5, v0, 1
          0x7E0C0280,              // v_mov_b32 v6, 0
          0xDCE8800C, 0x00000506,  // global_atomic_or v6, v5, s[0:1] offset:12
          0x4E0E00A0,              // v_subrev_nc_u32 v7, 32, v0
          0xDCDC8010, 0x00000706,  // global_atomic_smax v6, v7, s[0:1] offset:16
          0xBF8C3F70,              // s_waitcnt vmcnt(0)
          0xBF810000,              // s_endpgm
      },
      {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)}, memory, base);
  if (!r.ran) return;
  CHECK(r.memory[0] == 64 && r.memory[1] == 0 && r.memory[3] == 0xFFFFFFFFu && r.memory[4] == 31);
  uint64_t seen = 0;
  int winners = 0;
  bool others_ok = true;
  for (uint32_t l = 0; l < 64; ++l) {
    if (r.v(l, 1) < 64) seen |= 1ull << r.v(l, 1);  // every value before an add, once
    if (r.v(l, 3) == 0) ++winners;
    else others_ok = others_ok && r.v(l, 3) == r.memory[2];
  }
  CHECK(seen == ~0ull);
  CHECK(winners == 1 && others_ok && r.memory[2] >= 1 && r.memory[2] <= 64);
}
