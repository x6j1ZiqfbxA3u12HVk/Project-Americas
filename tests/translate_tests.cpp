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
#include "shader/image_bindings.h"
#include "shader/translate.h"
#include "test.h"

namespace {

using namespace am::shader;

struct Result {
  bool ran = false;
  bool structured = false;
  uint32_t nv = 0;
  std::vector<uint32_t> dump, memory, bindings;
  std::vector<std::vector<uint8_t>> textures;
  uint32_t s(int i) const { return dump[i]; }
  bool scc() const { return dump[128] != 0; }
  uint32_t v(int lane, int reg) const { return dump[kDumpLanes / 4 + lane * nv + reg]; }
};

// One wave64 group of 64 threads (rsrc2: 2 user SGPRs, group id x), or as given.
Result run(const std::vector<uint32_t>& code, std::vector<uint32_t> user = {}, std::vector<uint32_t> memory = {},
           uint64_t base = 0, uint32_t rsrc2 = (2u << 1) | (1u << 7), const am::test::ImageSetup* images = nullptr) {
  Result r;
  TranslateInput in;
  in.code = code;
  in.stage = HwStage::Cs;
  in.wave32 = false;
  in.vgprs = 12;
  in.lds_bytes = 16384;
  in.cs_rsrc2 = rsrc2;
  in.cs_threads[0] = 64;
  const Translation t = translate(in, Binding::Test);
  CHECK(t.ok);
  r.structured = t.structured;
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
  const am::test::GpuRun g = am::test::run_compute(c.dxil, 1, args, memory, kDumpLanes / 4 + 64 * r.nv, images);
  if (!g.ran) {
    std::printf("    skipped: %s\n", g.why.c_str());
    return r;
  }
  r.ran = true;
  r.dump = g.dump;
  r.memory = g.memory;
  r.bindings = g.bindings;
  r.textures = g.textures;
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
bool close_to(uint32_t got, float want) { return std::fabs(f(got) - want) <= 1e-6f; }
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
    ok = close_to(r.v(l, 4), u) && close_to(r.v(l, 5), u2) && f(r.v(l, 6)) == 1.0f && r.v(l, 7) == 0;
    ok = ok && f(r.v(l, 8)) == 1.0f && close_to(r.v(l, 9), u2) && close_to(r.v(l, 10), u) && f(r.v(l, 11)) == 1.0f;
    ok = ok && f(r.v(l, 12)) == static_cast<float>(l) && f(r.v(l, 13)) == -0.5f;
    ok = ok && close_to(r.v(l, 14), 16 * l / 1023.0f) && f(r.v(l, 15)) == 1.0f && r.v(l, 16) == 0 && f(r.v(l, 17)) == 1.0f;
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

namespace {
// A test's T# or S#: words that only have to be distinct (the host's table maps them).
void descriptor(std::vector<uint32_t>& memory, size_t at, uint32_t id, int words, uint32_t* out) {
  for (int k = 0; k < words; ++k) out[k] = memory[at + k] = 0xA0000000u | id << 8 | k;
}
// D3D's cube rules (the PC side's own): the face and the face coordinates of a direction.
void cube_face(float x, float y, float z, int& face, float& u, float& v) {
  const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
  float sc, tc, ma;
  if (ax >= ay && ax >= az) {
    face = x > 0 ? 0 : 1;
    sc = x > 0 ? -z : z;
    tc = -y;
    ma = ax;
  } else if (ay >= az) {
    face = y > 0 ? 2 : 3;
    sc = x;
    tc = y > 0 ? z : -z;
    ma = ay;
  } else {
    face = z > 0 ? 4 : 5;
    sc = z > 0 ? x : -x;
    tc = -y;
    ma = az;
  }
  u = (sc / ma + 1) / 2;
  v = (tc / ma + 1) / 2;
}
std::vector<uint8_t> bytes_of(const std::vector<uint32_t>& v) {
  std::vector<uint8_t> d(v.size() * 4);
  std::memcpy(d.data(), v.data(), d.size());
  return d;
}
template <typename F>
std::vector<uint8_t> texels(uint32_t w, uint32_t h, F texel) {
  std::vector<uint32_t> d(w * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) d[y * w + x] = texel(x, y);
  return bytes_of(d);
}
uint32_t bits(float x) {
  uint32_t u;
  std::memcpy(&u, &x, 4);
  return u;
}
}  // namespace

TEST_CASE(translate_images) {
  using am::test::TextureSpec;
  namespace sh = am::shader;
  const uint64_t base = 0x400000000ull;
  std::vector<uint32_t> memory(512, 0);
  std::vector<sh::ImageBinding> ib(6);
  std::vector<sh::SamplerBinding> sb(2);
  uint32_t missing[8];
  // T#s at 0x00 .. 0x80 and 0xE0, S#s at 0xA0 and 0xB0, a T# the table lacks at 0xC0.
  const size_t at[] = {0x00 / 4, 0x20 / 4, 0x40 / 4, 0x60 / 4, 0x80 / 4, 0xE0 / 4};
  for (uint32_t i = 0; i < 6; ++i) {
    descriptor(memory, at[i], i, 8, ib[i].words);
    ib[i].srv = 3 * i;
    ib[i].alt = 3 * i + 1;
    ib[i].uav = 3 * i + 2;
  }
  descriptor(memory, 0xA0 / 4, 0x10, 4, sb[0].words);
  descriptor(memory, 0xB0 / 4, 0x11, 4, sb[1].words);
  descriptor(memory, 0xC0 / 4, 0x20, 8, missing);
  sb[0].index = 0;
  sb[1].index = 1;
  sb[1].flags = sh::kSamplerCompare;
  ib[1].flags = sh::kImageUint | sh::kImageHasUav | sh::kImageLoadUav;
  ib[4].flags = sh::kImageUint | sh::kImageArray;
  ib[5].flags = sh::kImageUint | sh::kImageHasUav | sh::kImageLoadUav;
  // Per-lane directions for the cube, off the face centres.
  for (uint32_t l = 0; l < 64; ++l) {
    const float a = (l / 6) % 2 ? 0.4f : -0.4f, b = (l / 12) % 2 ? 0.3f : -0.3f;
    float d[3];
    switch (l % 6) {
      case 0: d[0] = 1; d[1] = a; d[2] = b; break;
      case 1: d[0] = -1; d[1] = a; d[2] = b; break;
      case 2: d[0] = a; d[1] = 1; d[2] = b; break;
      case 3: d[0] = a; d[1] = -1; d[2] = b; break;
      case 4: d[0] = a; d[1] = b; d[2] = 1; break;
      default: d[0] = a; d[1] = b; d[2] = -1; break;
    }
    std::memcpy(&memory[0x400 / 4 + 3 * l], d, 12);
  }

  am::test::ImageSetup setup;
  TextureSpec t0;  // 4x4 RGBA8: (16 x, 16 y, 7, 255)
  t0.width = t0.height = 4;
  t0.data = {texels(4, 4, [](uint32_t x, uint32_t y) { return (16 * x) | (16 * y) << 8 | 7u << 16 | 255u << 24; })};
  TextureSpec t1;  // 8x8 R32_UINT, written
  t1.format = DXGI_FORMAT_R32_UINT;
  t1.width = t1.height = 8;
  t1.uav = true;
  TextureSpec t2;  // 2x2 R32_FLOAT with two levels: 1.0, then 5.0
  t2.format = DXGI_FORMAT_R32_FLOAT;
  t2.width = t2.height = 2;
  t2.mips = 2;
  t2.data = {bytes_of({bits(1.0f), bits(1.0f), bits(1.0f), bits(1.0f)}), bytes_of({bits(5.0f)})};
  TextureSpec t3;  // a cube of 2x2 faces: (40 face, 50 + 100 i, 50 + 100 j, 255)
  t3.kind = TextureSpec::Cube;
  t3.width = t3.height = 2;
  t3.depth_or_layers = 6;
  for (uint32_t face = 0; face < 6; ++face) {
    t3.data.push_back(texels(2, 2, [face](uint32_t i, uint32_t j) { return (40 * face) | (50 + 100 * i) << 8 | (50 + 100 * j) << 16 | 255u << 24; }));
  }
  TextureSpec t4;  // 2x2x3 R32_UINT array: 1000 layer + 10 y + x + 1
  t4.format = DXGI_FORMAT_R32_UINT;
  t4.width = t4.height = 2;
  t4.depth_or_layers = 3;
  t4.array = true;
  for (uint32_t z = 0; z < 3; ++z) t4.data.push_back(bytes_of({1000 * z + 1, 1000 * z + 2, 1000 * z + 11, 1000 * z + 12}));
  TextureSpec t5;  // 1x1 R32_UINT for atomics
  t5.format = DXGI_FORMAT_R32_UINT;
  t5.uav = true;
  setup.textures = {t0, t1, t2, t3, t4, t5};
  D3D12_SAMPLER_DESC point{};
  point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  point.MaxAnisotropy = 1;
  point.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_SAMPLER_DESC cmp = point;
  cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  setup.samplers = {point, cmp};
  setup.bindings = sh::build_bindings(ib, sb, 8);
  CHECK(!setup.bindings.empty());

  const Result r = run(
      {
          0xF40C0200, 0xFA000000,              // s_load_dwordx8 s[8:15], s[0:1], 0x0     T0
          0xF40C0400, 0xFA000020,              // s_load_dwordx8 s[16:23], s[0:1], 0x20   T1
          0xF40C0600, 0xFA000040,              // s_load_dwordx8 s[24:31], s[0:1], 0x40   T2
          0xF40C0800, 0xFA000060,              // s_load_dwordx8 s[32:39], s[0:1], 0x60   T3 (cube)
          0xF40C0A00, 0xFA000080,              // s_load_dwordx8 s[40:47], s[0:1], 0x80   T4 (array)
          0xF4080C00, 0xFA0000A0,              // s_load_dwordx4 s[48:51], s[0:1], 0xa0   S0 (point)
          0xF4080D00, 0xFA0000B0,              // s_load_dwordx4 s[52:55], s[0:1], 0xb0   S1 (compare)
          0xF40C0E00, 0xFA0000C0,              // s_load_dwordx8 s[56:63], s[0:1], 0xc0   not in the table
          0xF40C1000, 0xFA0000E0,              // s_load_dwordx8 s[64:71], s[0:1], 0xe0   T5 (atomics)
          0xBF8CC07F,                          // s_waitcnt lgkmcnt(0)
          0x36020087,                          // v_and_b32 v1, 7, v0                   x
          0x2C040083,                          // v_lshrrev_b32 v2, 3, v0               y
          0x36060283,                          // v_and_b32 v3, 3, v1
          0x36080483,                          // v_and_b32 v4, 3, v2
          0xF0001F08, 0x00020503,              // image_load v[5:8], v[3:4], s[8:15] dmask:0xf dim:2D unorm
          0x16120083,                          // v_mul_u32_u24 v9, 3, v0
          0xF0201108, 0x00040901,              // image_store v9, v[1:2], s[16:23] dmask:0x1 dim:2D unorm
          0xF0001108, 0x00040A01,              // image_load v10, v[1:2], s[16:23] dmask:0x1 dim:2D unorm
          0x7E1602F0,                          // v_mov_b32 v11, 0.5
          0x7E1802F0,                          // v_mov_b32 v12, 0.5
          0x7E1A02F2,                          // v_mov_b32 v13, 1.0
          0xF0900108, 0x01860E0B,              // image_sample_l v14, v[11:13], s[24:31], s[48:51] dmask:0x1 dim:2D
          0xF09C0108, 0x01860F0B,              // image_sample_lz v15, v[11:12], s[24:31], s[48:51] dmask:0x1 dim:2D
          0x7E2202F0,                          // v_mov_b32 v17, 0.5
          0xF0BC010A, 0x01A61011, 0x00000C0B,  // image_sample_c_lz v16, [v17, v11, v12], s[24:31], s[52:55] dmask:0x1 dim:2D
          0x162E008C,                          // v_mul_u32_u24 v23, 12, v0
          0xDC3C8400, 0x14000017,              // global_load_dwordx3 v[20:22], v23, s[0:1] offset:1024
          0xBF8C3F70,                          // s_waitcnt vmcnt(0)
          0xD5470018, 0x045A2B14,              // v_cubema_f32 v24, v20, v21, v22
          0xD5450019, 0x045A2B14,              // v_cubesc_f32 v25, v20, v21, v22
          0xD546001A, 0x045A2B14,              // v_cubetc_f32 v26, v20, v21, v22
          0xD544001B, 0x045A2B14,              // v_cubeid_f32 v27, v20, v21, v22
          0xD5AA0118, 0x02010118,              // v_rcp_f32_e64 v24, |v24|
          0x5A323119, 0x3FC00000,              // v_fmaak_f32 v25, v25, v24, 0x3fc00000   s = sc / |ma| + 1.5
          0x5A34311A, 0x3FC00000,              // v_fmaak_f32 v26, v26, v24, 0x3fc00000
          0xF09C0F18, 0x01881C19,              // image_sample_lz v[28:31], v[25:27], s[32:39], s[48:51] dmask:0xf dim:CUBE
          0x2C460084,                          // v_lshrrev_b32 v35, 4, v0               layer 0..3 (3 is out of range)
          0x36420281,                          // v_and_b32 v33, 1, v1
          0x36440481,                          // v_and_b32 v34, 1, v2
          0xF0001128, 0x000A2421,              // image_load v36, v[33:35], s[40:47] dmask:0x1 dim:2D_ARRAY unorm
          0x7E520281,                          // v_mov_b32 v41, 1
          0xF0381F08, 0x00062529,              // image_get_resinfo v[37:40], v41, s[24:31] dmask:0xf dim:2D unorm
          0xF11C0208, 0x01822A0B,              // image_gather4_lz v[42:45], v[11:12], s[8:15], s[48:51] dmask:0x2 dim:2D
          0xF0001108, 0x000E2E01,              // image_load v46, v[1:2], s[56:63] dmask:0x1 dim:2D unorm
          0x7E5E0281,                          // v_mov_b32 v47, 1
          0x7E600280,                          // v_mov_b32 v48, 0
          0x7E620280,                          // v_mov_b32 v49, 0
          0xF0443108, 0x00102F30,              // image_atomic_add v47, v[48:49], s[64:71] dmask:0x1 dim:2D unorm glc
          0xBF8C3F70,                          // s_waitcnt vmcnt(0)
          0xBF810000,                          // s_endpgm
      },
      {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)}, memory, base, (2u << 1) | (1u << 7), &setup);
  if (!r.ran) return;
  const auto unorm = [](uint32_t b) { return b / 255.0f; };
  bool ok = true;
  uint64_t seen = 0;
  for (uint32_t l = 0; l < 64 && ok; ++l) {
    const uint32_t x = l & 7, y = l >> 3;
    ok = close_to(r.v(l, 5), unorm(16 * (x & 3))) && close_to(r.v(l, 6), unorm(16 * (y & 3))) && close_to(r.v(l, 7), unorm(7)) &&
         f(r.v(l, 8)) == 1.0f;
    if (!ok) std::printf("    lane %u: load %08x %08x %08x %08x\n", l, r.v(l, 5), r.v(l, 6), r.v(l, 7), r.v(l, 8));
    ok = ok && r.v(l, 10) == 3 * l;
    ok = ok && f(r.v(l, 14)) == 5.0f && f(r.v(l, 15)) == 1.0f && f(r.v(l, 16)) == 1.0f;
    if (!ok) {
      std::printf("    lane %u: uav load %u, sample_l %08x, lz %08x, compare %08x\n", l, r.v(l, 10), r.v(l, 14), r.v(l, 15),
                  r.v(l, 16));
    }
    float d[3];
    std::memcpy(d, &memory[0x400 / 4 + 3 * l], 12);
    int face;
    float u, v;
    cube_face(d[0], d[1], d[2], face, u, v);
    const uint32_t i = u >= 0.5f ? 1 : 0, j = v >= 0.5f ? 1 : 0;
    const bool cube = close_to(r.v(l, 28), unorm(40 * face)) && close_to(r.v(l, 29), unorm(50 + 100 * i)) &&
                      close_to(r.v(l, 30), unorm(50 + 100 * j));
    if (!cube) {
      std::printf("    lane %u: cube (%g %g %g) face %d texel %u,%u: got %08x %08x %08x\n", l, d[0], d[1], d[2], face, i, j,
                  r.v(l, 28), r.v(l, 29), r.v(l, 30));
    }
    ok = ok && cube;
    const uint32_t layer = l >> 4;
    const uint32_t want = layer < 3 ? 1000 * layer + 10 * (y & 1) + (x & 1) + 1 : 0u;
    ok = ok && r.v(l, 36) == want;
    if (!ok) std::printf("    lane %u: array load %u, not %u\n", l, r.v(l, 36), want);
    ok = ok && r.v(l, 37) == 1 && r.v(l, 38) == 1 && r.v(l, 39) == 1 && r.v(l, 40) == 2;
    // Gather of green at the centre of the 4x4: texels (1,2) (2,2) (2,1) (1,1).
    ok = ok && close_to(r.v(l, 42), unorm(32)) && close_to(r.v(l, 43), unorm(32)) && close_to(r.v(l, 44), unorm(16)) &&
         close_to(r.v(l, 45), unorm(16));
    if (!ok) {
      std::printf("    lane %u: resinfo %u %u %u %u gather %08x %08x %08x %08x\n", l, r.v(l, 37), r.v(l, 38), r.v(l, 39),
                  r.v(l, 40), r.v(l, 42), r.v(l, 43), r.v(l, 44), r.v(l, 45));
    }
    ok = ok && r.v(l, 46) == 0;
    if (r.v(l, 47) < 64) seen |= 1ull << r.v(l, 47);
  }
  CHECK(ok);
  CHECK(seen == ~0ull);
  // What the store and the atomics left in the textures.
  bool stored = r.textures.size() == 6 && r.textures[1].size() == 8 * 8 * 4;
  for (uint32_t l = 0; stored && l < 64; ++l) {
    uint32_t w;
    std::memcpy(&w, &r.textures[1][4 * l], 4);
    stored = w == 3 * l;
  }
  CHECK(stored);
  uint32_t count = 0;
  if (r.textures.size() == 6) std::memcpy(&count, r.textures[5].data(), 4);
  CHECK(count == 64);
  // The missing T# was queued for the host.
  const uint32_t misses_at = r.bindings[4] / 4;
  CHECK(r.bindings[6] >= 1 && r.bindings[misses_at] == 1 && std::memcmp(&r.bindings[misses_at + 4], missing, 32) == 0);
}

TEST_CASE(translate_structured_control_flow) {
  const Result r = run({
      0xBE800380,  // s_mov_b32 s0, 0                 i
      0xBE820380,  // s_mov_b32 s2, 0                 iterations
      0xBE830380,  // s_mov_b32 s3, 0
      0xBE850380,  // s_mov_b32 s5, 0
      // outer:
      0xBE810380,  // s_mov_b32 s1, 0                 j
      // inner:
      0x80028102,  // s_add_u32 s2, s2, 1
      0xBF068301,  // s_cmp_eq_u32 s1, 3
      0xBF840002,  // s_cbranch_scc0 skip
      0xBF068400,  // s_cmp_eq_u32 s0, 4
      0xBF850008,  // s_cbranch_scc1 done            out of both loops
      // skip:
      0x80018101,  // s_add_u32 s1, s1, 1
      0xBF0A8501,  // s_cmp_lt_u32 s1, 5
      0xBF85FFF8,  // s_cbranch_scc1 inner
      0x80008100,  // s_add_u32 s0, s0, 1
      0xBF0A8A00,  // s_cmp_lt_u32 s0, 10
      0xBF85FFF4,  // s_cbranch_scc1 outer
      0xBE8303FF, 0x0000006F,  // s_mov_b32 s3, 111   (only if the loops end normally)
      // done:
      0xBE840380,  // s_mov_b32 s4, 0
      // count:
      0x80048104,  // s_add_u32 s4, s4, 1
      0x87068104,  // s_and_b32 s6, s4, 1
      0xBF068006,  // s_cmp_eq_u32 s6, 0
      0xBF850001,  // s_cbranch_scc1 count_next      (continue)
      0x80050405,  // s_add_u32 s5, s5, s4
      // count_next:
      0xBF0A8904,  // s_cmp_lt_u32 s4, 9
      0xBF85FFF9,  // s_cbranch_scc1 count
      0xBF089405,  // s_cmp_gt_u32 s5, 20
      0xBF840002,  // s_cbranch_scc0 small
      0xBE870381,  // s_mov_b32 s7, 1
      0xBF820001,  // s_branch joined
      // small:
      0xBE870382,  // s_mov_b32 s7, 2
      // joined:
      0x7E020285,  // v_mov_b32 v1, 5
      0xBE88047E,  // s_mov_b64 s[8:9], exec
      0xBEFE0480,  // s_mov_b64 exec, 0
      0xBF880002,  // s_cbranch_execz after
      0x7E0202FF, 0x00000063,  // v_mov_b32 v1, 99
      // after:
      0xBEFE0408,  // s_mov_b64 exec, s[8:9]
      0x7D88008A,  // v_cmp_gt_u32 vcc, 10, v0
      0xBE8A246A,  // s_and_saveexec_b64 s[10:11], vcc
      0xBF880001,  // s_cbranch_execz no_lanes
      0x7E040287,  // v_mov_b32 v2, 7
      // no_lanes:
      0xBEFE040A,  // s_mov_b64 exec, s[10:11]
      0xBF810000,  // s_endpgm
  });
  CHECK(r.structured);
  if (!r.ran) return;
  CHECK(r.s(0) == 4 && r.s(1) == 3 && r.s(2) == 24 && r.s(3) == 0);  // 4 x 5 + 4 iterations, then out of both
  CHECK(r.s(4) == 9 && r.s(5) == 25 && r.s(7) == 1);                   // 1 + 3 + 5 + 7 + 9
  CHECK(r.s(126) == 0xFFFFFFFFu && r.s(127) == 0xFFFFFFFFu);
  bool ok = true;
  for (int l = 0; l < 64; ++l) ok = ok && r.v(l, 1) == 5 && r.v(l, 2) == (l < 10 ? 7u : 0u);
  CHECK(ok);
}

// Two ways into one loop: not reducible, so the dispatcher runs it.
TEST_CASE(translate_irreducible_control_flow) {
  const std::vector<uint32_t> code = {
      0xBE810380,  // s_mov_b32 s1, 0
      0xBF068000,  // s_cmp_eq_u32 s0, 0
      0xBF850001,  // s_cbranch_scc1 b
      // a:
      0x80018101,  // s_add_u32 s1, s1, 1
      // b:
      0x80018201,  // s_add_u32 s1, s1, 2
      0xBF0A8A01,  // s_cmp_lt_u32 s1, 10
      0xBF85FFFC,  // s_cbranch_scc1 a
      0xBF810000,  // s_endpgm
  };
  const Result in_at_b = run(code, {0});
  CHECK(!in_at_b.structured);
  if (in_at_b.ran) CHECK(in_at_b.s(1) == 11);
  const Result in_at_a = run(code, {1});
  if (in_at_a.ran) CHECK(in_at_a.s(1) == 12);
}

// The bindings table on the CPU: every descriptor in one of its two slots, as the program reads it.
TEST_CASE(image_bindings_table) {
  namespace sh = am::shader;
  std::vector<sh::ImageBinding> images(3000);
  std::vector<sh::SamplerBinding> samplers(500);
  uint32_t x = 12345;
  const auto next = [&x] { return x = x * 1664525u + 1013904223u; };
  for (size_t i = 0; i < images.size(); ++i) {
    for (uint32_t& w : images[i].words) w = next();
    images[i].srv = static_cast<uint32_t>(3 * i);
    images[i].flags = static_cast<uint32_t>(i & 7);
  }
  for (size_t i = 0; i < samplers.size(); ++i) {
    for (uint32_t& w : samplers[i].words) w = next();
    samplers[i].index = static_cast<uint32_t>(i);
  }
  images.push_back(images[17]);  // a descriptor twice: the first entry stands
  images.back().srv = 999999;
  const std::vector<uint32_t> t = sh::build_bindings(images, samplers, 16);
  CHECK(!t.empty());
  if (t.empty()) return;
  const auto find = [&](uint32_t table, uint32_t cap, uint32_t size, const uint32_t* key, int words) -> const uint32_t* {
    uint32_t k[8] = {};
    std::memcpy(k, key, 4 * words);
    const uint32_t h = sh::binding_hash(std::span<const uint32_t, 8>(k, 8));
    for (const uint32_t slot : {h & (cap - 1), sh::binding_hash2(h) & (cap - 1)}) {
      const uint32_t* e = &t[(table + slot * size) / 4];
      if (e[11] != 0 && std::memcmp(e, k, 32) == 0) return e;
    }
    return nullptr;
  };
  bool ok = true;
  for (size_t i = 0; i < 3000 && ok; ++i) {
    const uint32_t* e = find(t[0], t[1], 64, images[i].words, 8);
    ok = e && e[8] == 3 * i && e[11] == ((i & 7) | sh::kBindingValid);
  }
  CHECK(ok);
  for (size_t i = 0; i < samplers.size() && ok; ++i) {
    const uint32_t* e = find(t[2], t[3], 48, samplers[i].words, 4);
    ok = e && e[8] == i;
  }
  CHECK(ok);
  CHECK(t[5] == 16 && t[6] == 0 && t[4] + 16 * 48 == t.size() * 4);  // the miss list: 16 records, none yet
}

TEST_CASE(translate_lds_atomics_st64_and_swizzle) {
  const Result r = run({
      0x7E020280,              // v_mov_b32 v1, 0
      0x7E040281,              // v_mov_b32 v2, 1
      0xD8340000, 0x00000101,  // ds_write_b32 v1, v1                      lds[0] = 0
      0xD8380201, 0x00010101,  // ds_write2_b32 v1, v1, v1 offset0:1 offset1:2   lds[1] = lds[2] = 0
      0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
      0xBF8A0000,              // s_barrier
      0xD8000000, 0x00000201,  // ds_add_u32 v1, v2
      0xD8800004, 0x03000201,  // ds_add_rtn_u32 v3, v1, v2 offset:4
      0x34080082,              // v_lshlrev_b32 v4, 2, v0
      0xD83C0201, 0x00020004,  // ds_write2st64_b32 v4, v0, v2 offset0:1 offset1:2
      0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
      0xD8E00102, 0x05000004,  // ds_read2st64_b32 v[5:6], v4 offset0:2 offset1:1
      0xD8A00008, 0x07000001,  // ds_max_rtn_u32 v7, v1, v0 offset:8
      0xD8D4003E, 0x08000000,  // ds_swizzle_b32 v8, v0 offset:swizzle(BITMASK_PERM,"pppp1")
      0xD8D4801B, 0x09000000,  // ds_swizzle_b32 v9, v0 offset:swizzle(QUAD_PERM,3,2,1,0)
      0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
      0xBF8A0000,              // s_barrier
      0xD8D80000, 0x0A000001,  // ds_read_b32 v10, v1
      0xD8D80004, 0x0B000001,  // ds_read_b32 v11, v1 offset:4
      0xD8D80008, 0x0C000001,  // ds_read_b32 v12, v1 offset:8
      0xBF8CC07F,              // s_waitcnt lgkmcnt(0)
      0xBF810000,              // s_endpgm
  });
  if (!r.ran) return;
  bool ok = true;
  uint64_t seen = 0;
  for (uint32_t l = 0; l < 64 && ok; ++l) {
    ok = r.v(l, 5) == 1 && r.v(l, 6) == l && r.v(l, 7) <= 63 && r.v(l, 8) == (l | 1) &&
         r.v(l, 9) == ((l & ~3u) | (3 - (l & 3))) && r.v(l, 10) == 64 && r.v(l, 11) == 64 && r.v(l, 12) == 63;
    if (!ok) {
      std::printf("    lane %u: v5 %u v6 %u v7 %u v8 %u v9 %u v10 %u v11 %u v12 %u\n", l, r.v(l, 5), r.v(l, 6), r.v(l, 7),
                  r.v(l, 8), r.v(l, 9), r.v(l, 10), r.v(l, 11), r.v(l, 12));
    }
    if (r.v(l, 3) < 64) seen |= 1ull << r.v(l, 3);
  }
  CHECK(ok);
  CHECK(seen == ~0ull);  // each lane got a different value before its add
}
