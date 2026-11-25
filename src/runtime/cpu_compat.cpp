#include "runtime/cpu_compat.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#include "core/file_view.h"
#include "core/text.h"
#include "runtime/log.h"
#include "x86/code_scan.h"
#include "x86/decoder.h"

namespace am::rt {
namespace {

using host::CpuFeature;
using x86::Instruction;

// Bump when the decoder or the scan changes what they find.
constexpr int kCacheVersion = 1;

// The console instruction sets a PC may lack that we patch or emulate.
constexpr CpuFeature kHandled[] = {CpuFeature::SSE4A, CpuFeature::CLWB, CpuFeature::CLZERO, CpuFeature::RDPID,
                                   CpuFeature::RDPRU, CpuFeature::MONITORX};

bool handled(CpuFeature f) {
  for (CpuFeature h : kHandled) {
    if (h == f) return true;
  }
  return false;
}

// ---- the census, cached ----

struct Census {
  std::array<uint64_t, host::kCpuFeatureCount> uses{};
  std::vector<x86::Site> sites;
};

std::filesystem::path cache_file(const std::filesystem::path& dir, const MappedImage& m, uint64_t file_size) {
  std::string key = m.name + "-" + hex(file_size) + "-" + hex(m.info.timestamp, 8);
  if (m.info.codeview) key += "-" + hex_bytes(m.info.codeview->guid.data(), 16) + "-" + std::to_string(m.info.codeview->age);
  return dir / L"cpu" / widen(key + "-v" + std::to_string(kCacheVersion) + ".txt");
}

bool read_census(const std::filesystem::path& file, Census& c) {
  std::ifstream in(file);
  if (!in) return false;
  std::string tag;
  in >> tag;
  if (tag != "uses") return false;
  for (auto& u : c.uses) in >> u;
  uint32_t rva = 0, length = 0, feature = 0, confidence = 0;
  while (in >> rva >> length >> feature >> confidence) {
    if (feature >= host::kCpuFeatureCount) return false;
    c.sites.push_back({rva, static_cast<uint8_t>(length), static_cast<x86::Confidence>(confidence),
                       static_cast<CpuFeature>(feature)});
  }
  return in.eof();
}

void write_census(const std::filesystem::path& file, const Census& c) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::trunc);
  out << "uses";
  for (uint64_t u : c.uses) out << ' ' << u;
  out << '\n';
  for (const x86::Site& s : c.sites) {
    out << s.rva << ' ' << unsigned{s.length} << ' ' << static_cast<unsigned>(s.feature) << ' '
        << static_cast<unsigned>(s.confidence) << '\n';
  }
}

bool census_of(const MappedImage& m, const std::filesystem::path& cache_dir, Census& c, bool& from_cache,
               std::string& error) {
  auto view = FileView::open(m.path, error);
  if (!view) return false;
  const std::filesystem::path file = cache_file(cache_dir, m, view->size());
  if (read_census(file, c)) {
    from_cache = true;
    return true;
  }
  c = Census{};
  const x86::CodeScan scan = x86::scan_code(m.info, view->bytes(), kHandled);
  for (size_t f = 0; f < host::kCpuFeatureCount; ++f) c.uses[f] = scan.by_feature[f][static_cast<size_t>(x86::Confidence::Confirmed)];
  c.sites = scan.sites;
  write_census(file, c);
  return true;
}

// ---- patching ----

void fill_nops(uint8_t* p, size_t n) {
  static const uint8_t kNops[10][10] = {
      {},
      {0x90},
      {0x66, 0x90},
      {0x0F, 0x1F, 0x00},
      {0x0F, 0x1F, 0x40, 0x00},
      {0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  while (n) {
    const size_t k = n > 9 ? 9 : n;
    std::memcpy(p, kNops[k], k);
    p += k;
    n -= k;
  }
}

// Rewrites one site in place when an equivalent of the same length exists.
bool patch_in_place(uint8_t* p, const Instruction& d, CpuFeature f) {
  switch (f) {
    case CpuFeature::CLWB:  // write-back is a hint; PC memory is coherent
      fill_nops(p, d.length);
      return true;
    case CpuFeature::MONITORX:
      if (d.modrm == 0xFA) {  // MONITORX: arm nothing
        fill_nops(p, d.length);
      } else {  // MWAITX: the wait loop around it keeps spinning, politely
        fill_nops(p, d.length - 3u);
        p[d.length - 3] = 0xF3;
        p[d.length - 2] = 0x90;
        p[d.length - 1] = 0x90;
      }
      return true;
    case CpuFeature::SSE4A:
      if (d.opcode == 0x2B && d.memory_operand()) {  // MOVNTSS / MOVNTSD -> MOVSS / MOVSD (same operands)
        p[d.opcode_offset] = 0x11;
        return true;
      }
      return false;
    default:
      return false;
  }
}

// ---- stubs ----

// Code the patches jump to, within a rel32 jump of the image: one 64 KiB block after its end.
class StubArena {
 public:
  uint8_t* take(const MappedImage& image, size_t size) {
    if (!block_) {
      const uint64_t start = (reinterpret_cast<uint64_t>(image.base) + image.size + 0xFFFF) & ~0xFFFFull;
      for (uint64_t at = start; at < start + (1ull << 30) && !block_; at += 0x10000) {
        block_ = static_cast<uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(at), kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
      }
      if (!block_) return nullptr;
    }
    if (used_ + size > kBlock) return nullptr;
    uint8_t* p = block_ + used_;
    used_ = (used_ + size + 15) & ~size_t{15};
    return p;
  }
  void finish() {
    if (!block_) return;
    DWORD old = 0;
    VirtualProtect(block_, kBlock, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), block_, kBlock);
  }

 private:
  static constexpr size_t kBlock = 0x10000;
  uint8_t* block_ = nullptr;
  size_t used_ = 0;
};

bool fits_rel32(const uint8_t* from_end, const uint8_t* to) {
  const int64_t d = to - from_end;
  return d >= INT32_MIN && d <= INT32_MAX;
}

void put_rel32(uint8_t* at, const uint8_t* from_end, const uint8_t* to) {
  const int32_t d = static_cast<int32_t>(to - from_end);
  std::memcpy(at, &d, 4);
}

// The game's clearing loops, `clzero; add rax, 64; cmp rax, reg; jb loop`: the first 7 bytes
// become a jump to a stub that clears the line with eight stores (rcx saved on the stack; the
// flags it changes are set again by the add and the cmp), adds 64 and jumps back to the cmp.
bool is_clzero_loop(const uint8_t* p) {
  return p[0] == 0x0F && p[1] == 0x01 && p[2] == 0xFC && p[3] == 0x48 && p[4] == 0x83 && p[5] == 0xC0 &&
         p[6] == 0x40 && (p[7] & 0xFB) == 0x48 && p[8] == 0x39 && (p[9] & 0xC7) == 0xC0 && p[10] == 0x72 &&
         static_cast<int8_t>(p[11]) == -12;
}

bool patch_clzero_loop(const MappedImage& image, uint8_t* p, StubArena& stubs) {
  std::vector<uint8_t> stub = {0x51, 0x48, 0x89, 0xC1, 0x48, 0x83, 0xE1, 0xC0};  // push rcx; mov rcx, rax; and rcx, -64
  const uint8_t first[] = {0x48, 0xC7, 0x01, 0, 0, 0, 0};                       // mov qword [rcx], 0
  stub.insert(stub.end(), std::begin(first), std::end(first));
  for (uint8_t off = 8; off < 64; off += 8) {
    const uint8_t next[] = {0x48, 0xC7, 0x41, off, 0, 0, 0, 0};  // mov qword [rcx + off], 0
    stub.insert(stub.end(), std::begin(next), std::end(next));
  }
  const uint8_t tail[] = {0x59, 0x48, 0x83, 0xC0, 0x40, 0xE9, 0, 0, 0, 0};  // pop rcx; add rax, 64; jmp back
  stub.insert(stub.end(), std::begin(tail), std::end(tail));
  uint8_t* code = stubs.take(image, stub.size());
  if (!code) return false;
  uint8_t* back = p + 7;
  if (!fits_rel32(code + stub.size(), back) || !fits_rel32(p + 5, code)) return false;
  std::memcpy(code, stub.data(), stub.size());
  put_rel32(code + stub.size() - 4, code + stub.size(), back);
  p[0] = 0xE9;
  put_rel32(p + 1, p + 5, code);
  p[5] = 0x66;  // a two-byte NOP, never reached
  p[6] = 0x90;
  return true;
}

// ---- emulation ----

constexpr size_t kHitSlots = 1024;
std::atomic<uint64_t> g_hit_address[kHitSlots];
std::atomic<uint64_t> g_hit_count[kHitSlots];

void count_hit(uint64_t address) {
  size_t i = (address * 0x9E3779B97F4A7C15ull) >> 54;
  for (size_t probe = 0; probe < kHitSlots; ++probe, i = (i + 1) % kHitSlots) {
    uint64_t expected = 0;
    if (g_hit_address[i].load(std::memory_order_relaxed) == address ||
        g_hit_address[i].compare_exchange_strong(expected, address)) {
      const uint64_t n = g_hit_count[i].fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == 1 || n == 1000 || n == 1000000) {
        logf("cpu: emulated the instruction at 0x%llx (%llu times)", static_cast<unsigned long long>(address),
             static_cast<unsigned long long>(n));
      }
      return;
    }
  }
}

uint64_t* gpr(CONTEXT* c, int n) {
  uint64_t* const regs[16] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                              &c->R8,  &c->R9,  &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
  return regs[n & 15];
}

M128A* xmm(CONTEXT* c, int n) { return &c->FltSave.XmmRegisters[n & 15]; }

// Copies up to 15 code bytes; fewer when the instruction sits at the end of readable memory.
size_t read_code(const uint8_t* at, uint8_t* out) {
  size_t n = 0;
  __try {
    for (; n < 15; ++n) out[n] = at[n];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

bool guarded_store(uint64_t address, const void* data, size_t size) {
  __try {
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint64_t effective_address(const Instruction& d, const uint8_t* code, const CONTEXT* c) {
  int64_t disp = 0;
  if (d.disp_size == 1) disp = static_cast<int8_t>(code[d.disp_offset]);
  if (d.disp_size == 4) {
    int32_t v;
    std::memcpy(&v, code + d.disp_offset, 4);
    disp = v;
  }
  auto* cc = const_cast<CONTEXT*>(c);
  const bool rex_x = (d.rex & 2) != 0, rex_b = (d.rex & 1) != 0;
  uint64_t a = 0;
  if (d.rip_relative) {
    a = c->Rip + d.length + static_cast<uint64_t>(disp);
  } else {
    if (d.has_sib) {
      const int base = d.sib & 7;
      const int index = ((d.sib >> 3) & 7) | (rex_x ? 8 : 0);
      if (!(base == 5 && d.mod() == 0)) a += *gpr(cc, base | (rex_b ? 8 : 0));
      if (index != 4) a += *gpr(cc, index) << (d.sib >> 6);
    } else {
      a += *gpr(cc, d.rm() | (rex_b ? 8 : 0));
    }
    a += static_cast<uint64_t>(disp);
  }
  if (d.p67) a &= 0xFFFFFFFFull;
  if (d.segment == 0x65) a += __readgsqword(0x30);  // GS: this thread's TEB
  return a;
}

uint64_t bit_mask(unsigned length) { return length == 0 ? ~0ull : ((1ull << length) - 1); }

// Emulates one instruction; false when it is not one we handle.
bool emulate(CONTEXT* c, const Instruction& d, const uint8_t* code) {
  const x86::Requirement req = x86::requirement(d);
  if (!req.feature) return false;
  const int reg = d.reg() | ((d.rex & 4) ? 8 : 0);
  const int rm = d.rm() | ((d.rex & 1) ? 8 : 0);
  switch (*req.feature) {
    case CpuFeature::CLWB:
    case CpuFeature::MONITORX:  // MONITORX / MWAITX: nothing to arm, nothing to wait for
      return true;
    case CpuFeature::CLZERO: {
      uint64_t line = d.p67 ? (c->Rax & 0xFFFFFFFFull) : c->Rax;
      line &= ~63ull;
      static const uint8_t zeros[64] = {};
      return guarded_store(line, zeros, 64);
    }
    case CpuFeature::RDPID:
      *gpr(c, rm) = GetCurrentProcessorNumber();
      return true;
    case CpuFeature::RDPRU: {
      const uint32_t which = static_cast<uint32_t>(c->Rcx);
      const uint64_t v = (which <= 1) ? __rdtsc() : 0;  // MPERF / APERF: a steadily rising count
      c->Rax = v & 0xFFFFFFFFull;
      c->Rdx = v >> 32;
      return true;
    }
    case CpuFeature::SSE4A: {
      if (d.opcode == 0x2B) {  // MOVNTSS / MOVNTSD m, xmm
        const M128A* src = xmm(c, reg);
        const uint64_t value = src->Low;
        return guarded_store(effective_address(d, code, c), &value, d.rep == 0xF3 ? 4 : 8);
      }
      if (!d.has_modrm || d.mod() != 3) return false;
      if (d.opcode == 0x78 && d.rep == 0xF2) {  // INSERTQ xmm1, xmm2, length, index
        M128A* dst = xmm(c, reg);
        const M128A* src = xmm(c, rm);
        const unsigned length = code[d.imm_offset] & 63, index = code[d.imm_offset + 1] & 63;
        const uint64_t field = bit_mask(length) << index;
        dst->Low = (dst->Low & ~field) | ((src->Low << index) & field);
        return true;
      }
      if (d.opcode == 0x79 && d.rep == 0xF2) {  // INSERTQ xmm1, xmm2 (length and index in xmm2[77:64])
        M128A* dst = xmm(c, reg);
        const M128A* src = xmm(c, rm);
        const auto hi = static_cast<uint64_t>(src->High);
        const unsigned length = hi & 63, index = (hi >> 8) & 63;
        const uint64_t field = bit_mask(length) << index;
        dst->Low = (dst->Low & ~field) | ((src->Low << index) & field);
        return true;
      }
      if (d.opcode == 0x78 && d.p66) {  // EXTRQ xmm, length, index
        M128A* x = xmm(c, rm);
        const unsigned length = code[d.imm_offset] & 63, index = code[d.imm_offset + 1] & 63;
        x->Low = (x->Low >> index) & bit_mask(length);
        return true;
      }
      if (d.opcode == 0x79 && d.p66) {  // EXTRQ xmm1, xmm2 (length and index in xmm2[13:0])
        M128A* x = xmm(c, reg);
        const M128A* ctl = xmm(c, rm);
        const unsigned length = ctl->Low & 63, index = (ctl->Low >> 8) & 63;
        x->Low = (x->Low >> index) & bit_mask(length);
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

bool guarded_zero(uint64_t address, uint64_t size) {
  __try {
    std::memset(reinterpret_cast<void*>(address), 0, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The flags `cmp a, b` leaves.
uint32_t compare_flags(uint32_t eflags, uint64_t a, uint64_t b) {
  const uint64_t r = a - b;
  uint32_t f = eflags & ~0x8D5u;  // CF PF AF ZF SF OF
  if (a < b) f |= 0x1;
  if (!(__popcnt(static_cast<uint32_t>(r & 0xFF)) & 1)) f |= 0x4;
  if ((a ^ b ^ r) & 0x10) f |= 0x10;
  if (r == 0) f |= 0x40;
  if (r >> 63) f |= 0x80;
  if (((a ^ b) & (a ^ r)) >> 63) f |= 0x800;
  return f;
}

// The game clears memory with loops of `clzero; add rax, 64; cmp rax, reg; jb back`: one trap
// per cache line would cost millions of exceptions. A trap on such a loop's CLZERO runs the
// whole loop: it clears the lines, leaves rax and the flags as the loop would, and continues
// after the branch.
bool run_clzero_loop(CONTEXT* c, const uint8_t* code, size_t n) {
  if (n < 12 || code[0] != 0x0F || code[1] != 0x01 || code[2] != 0xFC) return false;
  if (code[3] != 0x48 || code[4] != 0x83 || code[5] != 0xC0 || code[6] != 0x40) return false;  // add rax, 64
  if ((code[7] & 0xFB) != 0x48 || code[8] != 0x39 || (code[9] & 0xC7) != 0xC0) return false;   // cmp rax, reg
  if (code[10] != 0x72 || static_cast<int8_t>(code[11]) != -12) return false;                  // jb to the clzero
  const int reg = ((code[9] >> 3) & 7) | ((code[7] & 4) ? 8 : 0);
  const uint64_t limit = *gpr(c, reg);
  uint64_t lines = 1;  // the loop runs its body before the test
  if (c->Rax + 64 < limit) lines = (limit - c->Rax + 63) / 64;
  if (!guarded_zero(c->Rax & ~63ull, lines * 64)) return false;
  c->Rax += lines * 64;
  c->EFlags = compare_flags(c->EFlags, c->Rax, limit);
  c->Rip += 12;
  return true;
}

LONG CALLBACK on_illegal_instruction(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  uint8_t code[16] = {};
  const size_t n = read_code(reinterpret_cast<const uint8_t*>(c->Rip), code);
  if (const uint64_t at = c->Rip; run_clzero_loop(c, code, n)) {
    count_hit(at);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  Instruction d;
  if (x86::decode(std::span<const uint8_t>(code, n), d) != x86::DecodeStatus::Ok) return EXCEPTION_CONTINUE_SEARCH;
  if (!emulate(c, d, code)) return EXCEPTION_CONTINUE_SEARCH;
  count_hit(c->Rip);
  c->Rip += d.length;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// The title's soft asserts: an int 3 followed by the code that carries on (a debugger would stop
// there and continue). A retail console without a debugger does not stop the title on them, so
// we step over each one, in title code only (our mapped images), and
// never into int 3 padding. Each site is logged once.
// (Not by asking Windows for the module: host import tables are redirected to answer for title
// images too.)
std::atomic<bool (*)(const void*)> g_is_title_code{nullptr};

LONG CALLBACK on_breakpoint(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  const auto is_title_code = g_is_title_code.load();
  if (!is_title_code || !is_title_code(reinterpret_cast<const void*>(c->Rip))) return EXCEPTION_CONTINUE_SEARCH;
  uint8_t code[16] = {};
  if (read_code(reinterpret_cast<const uint8_t*>(c->Rip), code) < 2 || code[0] != 0xCC || code[1] == 0xCC) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static std::mutex lock;
  static std::vector<uint64_t> seen;
  {
    std::lock_guard g(lock);
    if (std::find(seen.begin(), seen.end(), c->Rip) == seen.end() && seen.size() < 256) {
      seen.push_back(c->Rip);
      logf("cpu: the title's assert at 0x%s stepped over (int 3, no debugger)", hex(c->Rip).c_str());
    }
  }
  c->Rip += 1;
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void set_title_code_test(bool (*is_title_code)(const void* address)) { g_is_title_code = is_title_code; }

void install_cpu_emulation() {
  static std::once_flag once;
  std::call_once(once, [] {
    AddVectoredExceptionHandler(1, &on_illegal_instruction);
    AddVectoredExceptionHandler(1, &on_breakpoint);
  });
}

std::vector<EmulationHit> emulation_hits() {
  std::vector<EmulationHit> hits;
  for (size_t i = 0; i < kHitSlots; ++i) {
    const uint64_t a = g_hit_address[i].load(std::memory_order_relaxed);
    if (a) hits.push_back({a, g_hit_count[i].load(std::memory_order_relaxed)});
  }
  return hits;
}

bool patch_clzero_loop_at(MappedImage& image, uint32_t rva) {
  if (rva + 12 > image.size || !is_clzero_loop(image.base + rva)) return false;
  StubArena stubs;
  const bool ok = patch_clzero_loop(image, image.base + rva, stubs);
  stubs.finish();
  FlushInstructionCache(GetCurrentProcess(), image.base + rva, 12);
  return ok;
}

bool apply_cpu_compat(MappedImage& image, const std::filesystem::path& cache_dir, bool allow_missing,
                      CpuCompatResult& result, std::string& error) {
  const host::CpuidSnapshot cpu = host::read_cpuid();
  const std::vector<CpuFeature> missing = host::missing_console_features(cpu);
  if (missing.empty()) return true;  // a console-class CPU: nothing to do

  Census census;
  if (!census_of(image, cache_dir, census, result.from_cache, error)) return false;
  result.uses = census.uses;

  // Instruction sets we cannot stand in for.
  std::string unsupported;
  for (CpuFeature f : missing) {
    const uint64_t n = census.uses[static_cast<size_t>(f)];
    if (n && !handled(f)) {
      unsupported += (unsupported.empty() ? "" : ", ") + std::string(host::feature_info(f).name) + " (" +
                     std::to_string(n) + " uses)";
    }
  }
  if (!unsupported.empty()) {
    const std::string what = image.name + " needs CPU instruction sets this CPU lacks: " + unsupported;
    if (!allow_missing) {
      error = what;
      return false;
    }
    logf("cpu: %s; continuing anyway", what.c_str());
  }

  StubArena stubs;
  for (const x86::Site& s : census.sites) {
    if (host::has_feature(cpu, s.feature)) continue;
    uint8_t* p = image.base + s.rva;
    if (s.feature == CpuFeature::CLZERO && s.confidence == x86::Confidence::Confirmed && s.rva + 12 <= image.size &&
        is_clzero_loop(p) && patch_clzero_loop(image, p, stubs)) {
      ++result.patched;
      continue;
    }
    Instruction d;
    if (s.confidence != x86::Confidence::Confirmed ||
        x86::decode(std::span<const uint8_t>(p, s.length), d) != x86::DecodeStatus::Ok || d.length != s.length ||
        !patch_in_place(p, d, s.feature)) {
      ++result.emulated;
      continue;
    }
    ++result.patched;
  }
  stubs.finish();
  logf("cpu: %s: %zu site(s) patched, %zu left to emulation%s", image.name.c_str(), result.patched, result.emulated,
       result.from_cache ? " (census from cache)" : "");
  return true;
}

}  // namespace am::rt
