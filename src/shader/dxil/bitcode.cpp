// The LLVM 3.7 bitcode writer for our DXIL modules. Every record is unabbreviated (abbreviation
// id 3: code, count and operands as 6-bit VBR), so there is no BLOCKINFO and no abbreviation to
// keep in step with the reader; DXC's assembler reads it and writes its own, compact form into
// the container. Layout as LLVM 3.7 writes a module: attribute groups and sets, the type table,
// triple and data layout, globals and functions, the module's constants (all constants live
// there), metadata, the symbol table, then the entry function's body with relative value ids.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "shader/dxil/dxil.h"

namespace am::shader::dxil {
namespace {

// Block ids and record codes (LLVM 3.7's LLVMBitCodes.h).
enum : unsigned {
  kModuleBlock = 8, kParamAttrBlock = 9, kParamAttrGroupBlock = 10, kConstantsBlock = 11, kFunctionBlock = 12,
  kValueSymtabBlock = 14, kMetadataBlock = 15, kTypeBlock = 17,
};
enum : unsigned { kModVersion = 1, kModTriple = 2, kModDataLayout = 3, kModGlobalVar = 7, kModFunction = 8 };
enum : unsigned {
  kTypeNumEntry = 1, kTypeVoid = 2, kTypeFloat = 3, kTypeDouble = 4, kTypeLabel = 5, kTypeInteger = 7, kTypePointer = 8,
  kTypeHalf = 10, kTypeArray = 11, kTypeVector = 12, kTypeMetadata = 16, kTypeStructAnon = 18, kTypeStructName = 19,
  kTypeStructNamed = 20, kTypeFunction = 21,
};
enum : unsigned { kCstSetType = 1, kCstNull = 2, kCstUndef = 3, kCstInteger = 4, kCstFloat = 6, kCstAggregate = 7 };
enum : unsigned { kMdString = 1, kMdValue = 2, kMdNode = 3, kMdName = 4, kMdNamedNode = 10 };
enum : unsigned { kVstEntry = 1 };
enum : unsigned {
  kFnDeclareBlocks = 1, kFnBinop = 2, kFnCast = 3, kFnRet = 10, kFnBr = 11, kFnSwitch = 12, kFnUnreachable = 15,
  kFnPhi = 16, kFnAlloca = 19, kFnLoad = 20, kFnExtractVal = 26, kFnCmp2 = 28, kFnVSelect = 29, kFnCall = 34,
  kFnAtomicRmw = 38, kFnGep = 43, kFnStore = 44, kFnCmpXchg = 46,
};
enum : unsigned { kAttrGroupEntry = 3, kAttrEntry = 2 };
// Attribute kinds (bitcode numbers).
enum : unsigned { kAttrNoDuplicate = 12, kAttrNoUnwind = 18, kAttrReadNone = 20, kAttrReadOnly = 21 };
constexpr unsigned kWidth = 3;  // abbreviation id width inside blocks (ids 0-3 are all we use)

class Bits {
 public:
  void put(uint64_t v, unsigned w) {
    cur_ |= (v & ((w == 64 ? 0 : (1ull << w)) - 1)) << n_;
    n_ += w;
    if (n_ >= 32) {
      words_.push_back(static_cast<uint32_t>(cur_));
      cur_ >>= 32;
      n_ -= 32;
    }
  }
  void vbr(uint64_t v, unsigned w) {
    const uint64_t hi = 1ull << (w - 1);
    while (v >= hi) {
      put((v & (hi - 1)) | hi, w);
      v >>= w - 1;
    }
    put(v, w);
  }
  void align() {
    if (n_) {
      words_.push_back(static_cast<uint32_t>(cur_));
      cur_ = 0;
      n_ = 0;
    }
  }
  void enter(unsigned id) {
    put(1, width_);  // ENTER_SUBBLOCK
    vbr(id, 8);
    vbr(kWidth, 4);
    align();
    stack_.push_back({width_, words_.size()});
    words_.push_back(0);  // the block's length in words, set at its end
    width_ = kWidth;
  }
  void exit() {
    put(0, width_);  // END_BLOCK
    align();
    const auto [w, at] = stack_.back();
    stack_.pop_back();
    words_[at] = static_cast<uint32_t>(words_.size() - at - 1);
    width_ = w;
  }
  void record(unsigned code, const std::vector<uint64_t>& ops) {
    put(3, width_);  // UNABBREV_RECORD
    vbr(code, 6);
    vbr(ops.size(), 6);
    for (uint64_t o : ops) vbr(o, 6);
  }
  void text(unsigned code, std::string_view s, std::vector<uint64_t> head = {}) {
    for (char c : s) head.push_back(static_cast<uint8_t>(c));
    record(code, head);
  }
  std::vector<uint8_t> bytes() {
    align();
    std::vector<uint8_t> out(words_.size() * 4);
    if (!words_.empty()) std::memcpy(out.data(), words_.data(), out.size());
    return out;
  }

 private:
  struct Open {
    unsigned width;
    size_t at;
  };
  std::vector<uint32_t> words_;
  std::vector<Open> stack_;
  uint64_t cur_ = 0;
  unsigned n_ = 0;
  unsigned width_ = 2;
};

uint64_t sign_rotate(int64_t v) { return v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(-v) << 1) | 1u; }

uint64_t align_code(uint32_t bytes) {
  if (bytes == 0) return 0;
  uint64_t log = 0;
  while ((1u << log) < bytes) ++log;
  return log + 1;
}

unsigned attr_set(Attr a) {  // the attribute set index (1-based) of a declaration
  switch (a) {
    case Attr::ReadNone: return 1;
    case Attr::ReadOnly: return 2;
    case Attr::None: return 3;
    case Attr::NoDuplicate: return 4;
  }
  return 0;
}

}  // namespace

std::vector<uint8_t> write_bitcode(const Module& m) {
  Bits w;
  // 'BC' 0xC0DE
  w.put('B', 8);
  w.put('C', 8);
  w.put(0x0, 4);
  w.put(0xC, 4);
  w.put(0xE, 4);
  w.put(0xD, 4);
  w.enter(kModuleBlock);
  w.record(kModVersion, {1});  // relative value ids

  w.enter(kParamAttrGroupBlock);
  constexpr uint64_t kFunctionIndex = 0xFFFFFFFFu;
  w.record(kAttrGroupEntry, {1, kFunctionIndex, 0, kAttrNoUnwind, 0, kAttrReadNone});
  w.record(kAttrGroupEntry, {2, kFunctionIndex, 0, kAttrNoUnwind, 0, kAttrReadOnly});
  w.record(kAttrGroupEntry, {3, kFunctionIndex, 0, kAttrNoUnwind});
  w.record(kAttrGroupEntry, {4, kFunctionIndex, 0, kAttrNoDuplicate, 0, kAttrNoUnwind});
  w.exit();
  w.enter(kParamAttrBlock);
  for (uint64_t g = 1; g <= 4; ++g) w.record(kAttrEntry, {g});
  w.exit();

  const auto& types = m.types();
  w.enter(kTypeBlock);
  w.record(kTypeNumEntry, {types.size()});
  for (const TypeInfo& t : types) {
    switch (t.kind) {
      case TypeKind::Void: w.record(kTypeVoid, {}); break;
      case TypeKind::Label: w.record(kTypeLabel, {}); break;
      case TypeKind::Metadata: w.record(kTypeMetadata, {}); break;
      case TypeKind::Int: w.record(kTypeInteger, {t.n}); break;
      case TypeKind::Half: w.record(kTypeHalf, {}); break;
      case TypeKind::Float: w.record(kTypeFloat, {}); break;
      case TypeKind::Double: w.record(kTypeDouble, {}); break;
      case TypeKind::Pointer: w.record(kTypePointer, {t.sub[0], t.n}); break;
      case TypeKind::Array: w.record(kTypeArray, {t.n, t.sub[0]}); break;
      case TypeKind::Vector: w.record(kTypeVector, {t.n, t.sub[0]}); break;
      case TypeKind::Struct: {
        std::vector<uint64_t> ops{0};
        for (Type s : t.sub) ops.push_back(s);
        if (t.name.empty()) {
          w.record(kTypeStructAnon, ops);
        } else {
          w.text(kTypeStructName, t.name);
          w.record(kTypeStructNamed, ops);
        }
        break;
      }
      case TypeKind::Function: {
        std::vector<uint64_t> ops{0};
        for (Type s : t.sub) ops.push_back(s);
        w.record(kTypeFunction, ops);
        break;
      }
    }
  }
  w.exit();
  w.text(kModTriple, "dxil-ms-dx");
  w.text(kModDataLayout, "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64");

  // Value ids: globals, functions, constants, then the body's instructions.
  std::vector<uint32_t> id(m.value_count(), kNone);
  uint32_t next = 0;
  for (const auto& g : m.globals()) {
    if (g.used) id[g.value] = next++;
  }
  for (const auto& f : m.functions()) {
    if (f.used) id[f.value] = next++;
  }
  for (Value c : m.constants()) id[c] = next++;
  const auto vid = [&](Value v) {
    const uint32_t x = id[m.resolve(v)];
    if (x == kNone) {
      std::fprintf(stderr, "dxil: an operand with no id (value %u)\n", m.resolve(v));
      std::abort();
    }
    return x;
  };
  for (const auto& g : m.globals()) {
    if (!g.used) continue;
    // [type, (space << 2) | explicit type | constant, init, linkage, alignment, section, ...]
    const bool constant = g.init != kNone;
    w.record(kModGlobalVar, {g.type, (static_cast<uint64_t>(g.space) << 2) | 2u | (constant ? 1u : 0u), constant ? id[g.init] + 1u : 0u,
                             constant ? 3u : 0u, align_code(g.align), 0, 0, 0, 0, 0, 0, 0});
  }
  for (const auto& f : m.functions()) {
    if (!f.used) continue;
    w.record(kModFunction, {f.type, 0, f.body ? 0u : 1u, 0, f.body ? 0u : attr_set(f.attr), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  }

  w.enter(kConstantsBlock);
  Type cur_type = kNone;
  for (Value c : m.constants()) {
    const ValueInfo& v = m.value(c);
    if (v.type != cur_type) {
      cur_type = v.type;
      w.record(kCstSetType, {v.type});
    }
    switch (v.kind) {
      case ValueKind::Int: {
        if (v.bits == 0) {
          w.record(kCstNull, {});
          break;
        }
        const uint32_t bits = m.type(v.type).n;
        int64_t s = static_cast<int64_t>(v.bits);
        if (bits < 64 && (v.bits >> (bits - 1)) & 1u) s = static_cast<int64_t>(v.bits | ~((1ull << bits) - 1));
        w.record(kCstInteger, {sign_rotate(s)});
        break;
      }
      case ValueKind::Float:
        if (v.bits == 0) w.record(kCstNull, {});
        else w.record(kCstFloat, {v.bits});
        break;
      case ValueKind::Null: w.record(kCstNull, {}); break;
      case ValueKind::Undef: w.record(kCstUndef, {}); break;
      case ValueKind::Aggregate: {
        std::vector<uint64_t> ops;
        for (Value e : m.aggregate_elements(c)) ops.push_back(id[e]);
        w.record(kCstAggregate, ops);
        break;
      }
      default: break;
    }
  }
  w.exit();

  w.enter(kMetadataBlock);
  for (const auto& md : m.metadata()) {
    switch (md.kind) {
      case Module::MdInfo::String: w.text(kMdString, md.text); break;
      case Module::MdInfo::Value: w.record(kMdValue, {m.type_of(md.value), vid(md.value)}); break;
      case Module::MdInfo::Node: {
        std::vector<uint64_t> ops;
        for (Md o : md.ops) ops.push_back(o == kNone ? 0 : static_cast<uint64_t>(o) + 1);
        w.record(kMdNode, ops);
        break;
      }
    }
  }
  for (const auto& [name, nodes] : m.named()) {
    w.text(kMdName, name);
    std::vector<uint64_t> ops(nodes.begin(), nodes.end());
    w.record(kMdNamedNode, ops);
  }
  w.exit();

  w.enter(kValueSymtabBlock);
  for (const auto& g : m.globals()) {
    if (g.used) w.text(kVstEntry, g.name, {id[g.value]});
  }
  for (const auto& f : m.functions()) {
    if (f.used) w.text(kVstEntry, f.name, {id[f.value]});
  }
  w.exit();

  // The entry function's body.
  const auto& order = m.order();
  std::vector<uint32_t> bidx(m.block_count(), kNone);
  for (size_t i = 0; i < order.size(); ++i) bidx[order[i]] = static_cast<uint32_t>(i);
  const auto block_of = [&](uint32_t b) {
    if (bidx[b] == kNone) {
      std::fprintf(stderr, "dxil: a branch to an unreachable block\n");
      std::abort();
    }
    return static_cast<uint64_t>(bidx[b]);
  };
  const auto live = [&](const Inst& in) {
    return in.op != Op::Read && in.op != Op::Write && (in.result == kNone || m.resolve(in.result) == in.result);
  };
  // Every instruction's id first: phis refer to values made later.
  const uint32_t first = next;
  for (Block b : order) {
    const BlockInfo& bi = m.block_info(b);
    for (uint32_t ii : bi.phis) {
      const Inst& in = m.inst(ii);
      if (live(in)) id[in.result] = next++;
    }
    for (uint32_t ii : bi.insts) {
      const Inst& in = m.inst(ii);
      if (live(in) && in.result != kNone) id[in.result] = next++;
    }
  }
  next = first;
  w.enter(kFunctionBlock);
  w.record(kFnDeclareBlocks, {order.size()});
  std::vector<uint64_t> ops;
  const auto push = [&](Value v) { ops.push_back(static_cast<uint32_t>(next - vid(v))); };
  const auto push_typed = [&](Value v) {
    const uint32_t x = vid(v);
    ops.push_back(static_cast<uint32_t>(next - x));
    if (x >= next) ops.push_back(m.type_of(m.resolve(v)));
  };
  for (Block b : order) {
    const BlockInfo& bi = m.block_info(b);
    for (int pass = 0; pass < 2; ++pass) {
      for (uint32_t ii : pass == 0 ? bi.phis : bi.insts) {
        const Inst& in = m.inst(ii);
        if (!live(in)) continue;
        ops.clear();
        unsigned code = 0;
        switch (in.op) {
          case Op::Bin:
            push_typed(in.ops[0]);
            push(in.ops[1]);
            ops.push_back(in.code);
            if (in.aux) ops.push_back(in.aux);
            code = kFnBinop;
            break;
          case Op::Cast:
            push_typed(in.ops[0]);
            ops.push_back(in.type);
            ops.push_back(in.code);
            code = kFnCast;
            break;
          case Op::Cmp:
            push_typed(in.ops[0]);
            push(in.ops[1]);
            ops.push_back(in.code);
            if (in.aux) ops.push_back(in.aux);
            code = kFnCmp2;
            break;
          case Op::Select:
            push_typed(in.ops[1]);
            push(in.ops[2]);
            push_typed(in.ops[0]);
            code = kFnVSelect;
            break;
          case Op::Call:
            ops.push_back(0);
            ops.push_back(1u << 15);  // explicit function type
            ops.push_back(in.type);
            push_typed(in.ops[0]);
            for (size_t k = 1; k < in.ops.size(); ++k) push(in.ops[k]);
            code = kFnCall;
            break;
          case Op::Load:
            push_typed(in.ops[0]);
            ops.push_back(in.type);
            ops.push_back(align_code(in.aux));
            ops.push_back(0);
            code = kFnLoad;
            break;
          case Op::Store:
            push_typed(in.ops[0]);
            push_typed(in.ops[1]);
            ops.push_back(align_code(in.aux));
            ops.push_back(0);
            code = kFnStore;
            break;
          case Op::Gep:
            ops.push_back(1);  // inbounds
            ops.push_back(in.type);
            for (uint32_t o : in.ops) push_typed(o);
            code = kFnGep;
            break;
          case Op::Alloca:
            ops.push_back(in.type);
            ops.push_back(m.type_of(in.ops[0]));
            ops.push_back(vid(in.ops[0]));
            ops.push_back(align_code(in.aux) | (1u << 6));  // explicit type
            code = kFnAlloca;
            break;
          case Op::Extract:
            push_typed(in.ops[0]);
            for (size_t k = 1; k < in.ops.size(); ++k) ops.push_back(in.ops[k]);
            code = kFnExtractVal;
            break;
          case Op::Phi:
            ops.push_back(in.type);
            for (size_t k = 0; k < in.ops.size(); k += 2) {
              ops.push_back(sign_rotate(static_cast<int64_t>(next) - static_cast<int64_t>(vid(in.ops[k]))));
              ops.push_back(block_of(in.ops[k + 1]));
            }
            code = kFnPhi;
            break;
          case Op::AtomicRmw:
            push_typed(in.ops[0]);
            push(in.ops[1]);
            ops.insert(ops.end(), {in.code, 0, 6, 1});  // not volatile, seq_cst, across threads
            code = kFnAtomicRmw;
            break;
          case Op::CmpXchg:
            push_typed(in.ops[0]);
            push_typed(in.ops[1]);
            push(in.ops[2]);
            ops.insert(ops.end(), {0, 6, 1, 6, 0});  // not volatile, seq_cst, across threads, seq_cst, strong
            code = kFnCmpXchg;
            break;
          case Op::Br:
            ops.push_back(block_of(in.ops[0]));
            code = kFnBr;
            break;
          case Op::CondBr:
            ops.push_back(block_of(in.ops[1]));
            ops.push_back(block_of(in.ops[2]));
            push(in.ops[0]);
            code = kFnBr;
            break;
          case Op::Switch:
            ops.push_back(m.type_of(m.resolve(in.ops[0])));
            push(in.ops[0]);
            ops.push_back(block_of(in.ops[1]));
            for (size_t k = 2; k < in.ops.size(); k += 2) {
              ops.push_back(vid(in.ops[k]));
              ops.push_back(block_of(in.ops[k + 1]));
            }
            code = kFnSwitch;
            break;
          case Op::Ret: code = kFnRet; break;
          case Op::Unreachable: code = kFnUnreachable; break;
          default: break;
        }
        w.record(code, ops);
        if (in.result != kNone) ++next;
      }
    }
  }
  w.exit();
  w.exit();
  return w.bytes();
}

}  // namespace am::shader::dxil
