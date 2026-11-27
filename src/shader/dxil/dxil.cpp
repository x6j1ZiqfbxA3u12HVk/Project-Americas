#include "shader/dxil/dxil.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace am::shader::dxil {
namespace {

bool is_terminator(Op op) {
  return op == Op::Br || op == Op::CondBr || op == Op::Switch || op == Op::Ret || op == Op::Unreachable;
}

// A terminator's successors, one per edge.
template <typename F>
void each_successor(const Inst& t, F&& f) {
  switch (t.op) {
    case Op::Br: f(t.ops[0]); break;
    case Op::CondBr: f(t.ops[1]); f(t.ops[2]); break;
    case Op::Switch:
      f(t.ops[1]);
      for (size_t k = 3; k < t.ops.size(); k += 2) f(t.ops[k]);
      break;
    default: break;
  }
}

void key_u32(std::string& k, uint32_t v) { k.append(reinterpret_cast<const char*>(&v), 4); }

// Shader flags (DXIL's bits, as the validator computes them).
constexpr uint64_t kFlagDouble = 1ull << 2;
constexpr uint64_t kFlagDoubleExtensions = 1ull << 6;
constexpr uint64_t kFlagTiledResources = 1ull << 12;
constexpr uint64_t kFlagBarycentrics = 1ull << 22;
constexpr uint64_t kFlagRawBuffers = 1ull << 4;
constexpr uint64_t kFlagUavsEveryStage = 1ull << 16;
constexpr uint64_t kFlagWaveOps = 1ull << 19;
constexpr uint64_t kFlagInt64 = 1ull << 20;
constexpr uint64_t kFlagHeapResources = 1ull << 30;
constexpr uint64_t kFlagHeapSamplers = 1ull << 31;

}  // namespace

Module::Module() {
  void_ = intern({TypeKind::Void, 0, {}, {}});
  i1_ = int_type(1);
  i8_ = int_type(8);
  i32_ = int_type(32);
  i64_ = int_type(64);
  f16_ = intern({TypeKind::Half, 0, {}, {}});
  f32_ = intern({TypeKind::Float, 0, {}, {}});
  f64_ = intern({TypeKind::Double, 0, {}, {}});
  handle_ = named_struct("dx.types.Handle", {pointer(i8_)});
  res_bind_ = named_struct("dx.types.ResBind", {i32_, i32_, i32_, i8_});
  res_props_ = named_struct("dx.types.ResourceProperties", {i32_, i32_});
  main_ = declare("main", function(void_, {}), Attr::None);
  functions_[values_[main_].bits].body = true;
  block();
}

void Module::fail(const char* what) const {
  std::fprintf(stderr, "dxil: %s\n", what);
  std::abort();
}

Type Module::intern(TypeInfo t) {
  std::string k(1, static_cast<char>(t.kind));
  key_u32(k, t.n);
  for (Type s : t.sub) key_u32(k, s);
  if (t.kind == TypeKind::Struct && !t.name.empty()) k = "S" + t.name;  // named: one per name
  const auto it = type_keys_.find(k);
  if (it != type_keys_.end()) {
    need(types_[it->second].sub == t.sub, "a named struct made again with other members");
    return it->second;
  }
  types_.push_back(std::move(t));
  const Type id = static_cast<Type>(types_.size() - 1);
  type_keys_.emplace(std::move(k), id);
  return id;
}

Type Module::int_type(uint32_t bits) { return intern({TypeKind::Int, bits, {}, {}}); }
Type Module::pointer(Type pointee, uint32_t space) { return intern({TypeKind::Pointer, space, {pointee}, {}}); }
Type Module::array(Type element, uint32_t n) { return intern({TypeKind::Array, n, {element}, {}}); }
Type Module::vector(Type element, uint32_t n) { return intern({TypeKind::Vector, n, {element}, {}}); }

Type Module::function(Type result, const std::vector<Type>& params) {
  std::vector<Type> sub{result};
  sub.insert(sub.end(), params.begin(), params.end());
  return intern({TypeKind::Function, 0, std::move(sub), {}});
}

Type Module::named_struct(std::string_view name, const std::vector<Type>& members) {
  return intern({TypeKind::Struct, 0, members, std::string(name)});
}

bool Module::is_float(Type t) const {
  const TypeKind k = types_[t].kind;
  return k == TypeKind::Half || k == TypeKind::Float || k == TypeKind::Double;
}

Value Module::add_value(ValueKind k, Type t, uint64_t bits) {
  values_.push_back({k, t, bits});
  return static_cast<Value>(values_.size() - 1);
}

Value Module::constant(ValueKind k, Type t, uint64_t bits) {
  std::string key(1, static_cast<char>(k));
  key_u32(key, t);
  key.append(reinterpret_cast<const char*>(&bits), 8);
  const auto it = const_keys_.find(key);
  if (it != const_keys_.end()) return it->second;
  const Value v = add_value(k, t, bits);
  constants_.push_back(v);
  const_keys_.emplace(std::move(key), v);
  return v;
}

Value Module::const_int(Type t, uint64_t v) {
  need(is_int(t), "an integer constant of a type not an integer");
  const uint32_t bits = types_[t].n;
  if (bits < 64) v &= (1ull << bits) - 1;
  return constant(ValueKind::Int, t, v);
}

Value Module::f32c(float v) {
  uint32_t b = 0;
  std::memcpy(&b, &v, 4);
  return const_float(f32_, b);
}

Value Module::const_float(Type t, uint64_t bits) {
  need(is_float(t), "a float constant of a type not a float");
  return constant(ValueKind::Float, t, bits);
}

Value Module::undef(Type t) { return constant(ValueKind::Undef, t, 0); }
Value Module::null(Type t) { return constant(ValueKind::Null, t, 0); }
// Zero as the one constant a number type has for it (a phi of zeros is then trivial).
Value Module::zero(Type t) {
  if (is_int(t)) return const_int(t, 0);
  if (is_float(t)) return const_float(t, 0);
  return null(t);
}

Value Module::aggregate(Type t, const std::vector<Value>& elements) {
  std::string key("A");
  key_u32(key, t);
  for (Value e : elements) {
    need(is_constant(e), "an aggregate of values not constants");
    key_u32(key, e);
  }
  const auto it = const_keys_.find(key);
  if (it != const_keys_.end()) return it->second;
  aggregates_.push_back(elements);
  const Value v = add_value(ValueKind::Aggregate, t, aggregates_.size() - 1);
  constants_.push_back(v);
  const_keys_.emplace(std::move(key), v);
  return v;
}

bool Module::is_constant(Value v) const {
  const ValueKind k = values_[v].kind;
  return k == ValueKind::Int || k == ValueKind::Float || k == ValueKind::Null || k == ValueKind::Undef || k == ValueKind::Aggregate;
}

bool Module::constant_int(Value v, uint64_t& out) const {
  v = resolve(v);
  if (values_[v].kind == ValueKind::Int) {
    out = values_[v].bits;
    return true;
  }
  if (values_[v].kind == ValueKind::Null && is_int(values_[v].type)) {
    out = 0;
    return true;
  }
  return false;
}

Value Module::declare(std::string_view name, Type fn, Attr attr) {
  const std::string n(name);
  const auto it = function_names_.find(n);
  if (it != function_names_.end()) {
    need(functions_[it->second].type == fn, "a function declared again with another type");
    return functions_[it->second].value;
  }
  need(types_[fn].kind == TypeKind::Function, "a declaration of a type not a function");
  const Value v = add_value(ValueKind::Function, pointer(fn), functions_.size());
  functions_.push_back({n, fn, attr, v, false});
  function_names_.emplace(n, static_cast<uint32_t>(functions_.size() - 1));
  return v;
}

Value Module::global(std::string_view name, Type t, uint32_t space, uint32_t align, Value init) {
  need(init == kNone || (is_constant(init) && type_of(init) == t), "a global's initializer not a constant of its type");
  const Value v = add_value(ValueKind::Global, pointer(t, space), globals_.size());
  globals_.push_back({std::string(name), t, space, align, v, init});
  return v;
}

Md Module::md_string(std::string_view s) {
  const std::string k(s);
  const auto it = md_strings_.find(k);
  if (it != md_strings_.end()) return it->second;
  md_.push_back({MdInfo::String, k, 0, {}});
  const Md m = static_cast<Md>(md_.size() - 1);
  md_strings_.emplace(k, m);
  return m;
}

Md Module::md_value(Value v) {
  md_.push_back({MdInfo::Value, {}, v, {}});
  return static_cast<Md>(md_.size() - 1);
}

Md Module::md_node(const std::vector<Md>& ops) {
  md_.push_back({MdInfo::Node, {}, 0, ops});
  return static_cast<Md>(md_.size() - 1);
}

void Module::named_metadata(std::string_view name, const std::vector<Md>& nodes) { named_.emplace_back(std::string(name), nodes); }

Block Module::block() {
  blocks_.emplace_back();
  return static_cast<Block>(blocks_.size() - 1);
}

bool Module::terminated(Block b) const {
  const auto& in = blocks_[b].insts;
  return !in.empty() && is_terminator(insts_[in.back()].op);
}

uint32_t Module::add_inst(Inst i) {
  insts_.push_back(std::move(i));
  return static_cast<uint32_t>(insts_.size() - 1);
}

Value Module::emit(Op op, Type result, std::vector<uint32_t> ops, uint8_t code, uint32_t aux, Type type) {
  need(!finished_, "an instruction after finish()");
  need(cur_ < blocks_.size() && !terminated(cur_), "an instruction after its block's end");
  Inst in;
  in.op = op;
  in.code = code;
  in.aux = aux;
  in.type = type;
  in.ops = std::move(ops);
  const uint32_t index = static_cast<uint32_t>(insts_.size());
  if (result != void_) in.result = add_value(ValueKind::Inst, result, index);
  const Value r = in.result;
  add_inst(std::move(in));
  blocks_[cur_].insts.push_back(index);
  return r;
}

Value Module::bin(Bin op, Value a, Value b, uint32_t flags) {
  const Type t = type_of(a);
  need(t == type_of(b), "a binary operator on two types");
  if (is_float(t)) {
    need(op == Bin::Add || op == Bin::Sub || op == Bin::Mul || op == Bin::SDiv || op == Bin::SRem, "an integer operator on floats");
  } else {
    need(is_int(t), "a binary operator on a type not a number");
  }
  return emit(Op::Bin, t, {a, b}, static_cast<uint8_t>(op), flags, t);
}

Value Module::cast(Cast op, Value v, Type to) {
  const Type from = type_of(v);
  switch (op) {
    case Cast::Trunc: need(is_int(from) && is_int(to) && types_[to].n < types_[from].n, "trunc to a type not narrower"); break;
    case Cast::ZExt:
    case Cast::SExt: need(is_int(from) && is_int(to) && types_[to].n > types_[from].n, "an extension to a type not wider"); break;
    case Cast::FpToUi:
    case Cast::FpToSi: need(is_float(from) && is_int(to), "a float to integer cast of other types"); break;
    case Cast::UiToFp:
    case Cast::SiToFp: need(is_int(from) && is_float(to), "an integer to float cast of other types"); break;
    case Cast::FpTrunc:
    case Cast::FpExt: need(is_float(from) && is_float(to), "a float cast of types not floats"); break;
    case Cast::Bitcast: break;
    default: break;
  }
  return emit(Op::Cast, to, {v}, static_cast<uint8_t>(op), 0, to);
}

Value Module::cmp(Pred p, Value a, Value b, uint32_t flags) {
  const Type t = type_of(a);
  need(t == type_of(b), "a compare of two types");
  need(static_cast<uint8_t>(p) >= 32 ? is_int(t) : is_float(t), "a compare predicate of the other kind");
  return emit(Op::Cmp, i1_, {a, b}, static_cast<uint8_t>(p), static_cast<uint8_t>(p) >= 32 ? 0u : flags, i1_);
}

Value Module::select(Value c, Value a, Value b) {
  need(type_of(c) == i1_, "a select on a condition not i1");
  need(type_of(a) == type_of(b), "a select of two types");
  return emit(Op::Select, type_of(a), {c, a, b}, 0, 0, type_of(a));
}

Value Module::call(Value fn, const std::vector<Value>& args) {
  need(values_[fn].kind == ValueKind::Function, "a call of a value not a function");
  const Type ft = functions_[values_[fn].bits].type;
  const TypeInfo& t = types_[ft];
  need(t.sub.size() == args.size() + 1, "a call with the wrong number of arguments");
  for (size_t k = 0; k < args.size(); ++k) {
    if (type_of(args[k]) != t.sub[k + 1]) fail(("a call argument of the wrong type: " + functions_[values_[fn].bits].name).c_str());
  }
  std::vector<uint32_t> ops{fn};
  ops.insert(ops.end(), args.begin(), args.end());
  const Type result = t.sub[0];
  return emit(Op::Call, result, std::move(ops), 0, 0, ft);
}

Value Module::load(Type t, Value ptr, uint32_t align) {
  const TypeInfo& p = types_[type_of(ptr)];
  need(p.kind == TypeKind::Pointer && p.sub[0] == t, "a load through a pointer to another type");
  return emit(Op::Load, t, {ptr}, 0, align, t);
}

void Module::store(Value ptr, Value v, uint32_t align) {
  const TypeInfo& p = types_[type_of(ptr)];
  need(p.kind == TypeKind::Pointer && p.sub[0] == type_of(v), "a store through a pointer to another type");
  emit(Op::Store, void_, {ptr, v}, 0, align, type_of(v));
}

Value Module::gep(Type source, Value base, const std::vector<Value>& indices) {
  const TypeInfo bt = types_[type_of(base)];
  need(bt.kind == TypeKind::Pointer && bt.sub[0] == source, "a getelementptr on a pointer to another type");
  need(!indices.empty(), "a getelementptr without indices");
  Type t = source;
  for (size_t k = 1; k < indices.size(); ++k) {
    const TypeInfo ti = types_[t];
    if (ti.kind == TypeKind::Array || ti.kind == TypeKind::Vector) {
      t = ti.sub[0];
    } else if (ti.kind == TypeKind::Struct) {
      uint64_t c = 0;
      need(constant_int(indices[k], c) && c < ti.sub.size(), "a struct index not a constant member");
      t = ti.sub[c];
    } else {
      fail("a getelementptr into a type without members");
    }
  }
  std::vector<uint32_t> ops{base};
  ops.insert(ops.end(), indices.begin(), indices.end());
  return emit(Op::Gep, pointer(t, bt.n), std::move(ops), 0, 0, source);
}

Value Module::alloca_(Type t, uint32_t align) {
  const Block keep = cur_;
  const bool ended = terminated(0);
  uint32_t term = 0;
  if (ended) {
    term = blocks_[0].insts.back();
    blocks_[0].insts.pop_back();
  }
  cur_ = 0;
  const Value r = emit(Op::Alloca, pointer(t), {u32(1)}, 0, align, t);
  if (ended) blocks_[0].insts.push_back(term);
  cur_ = keep;
  return r;
}

Value Module::extract(Value aggregate, uint32_t index) {
  const TypeInfo t = types_[type_of(aggregate)];
  need((t.kind == TypeKind::Struct || t.kind == TypeKind::Array) && (t.kind == TypeKind::Array ? index < t.n : index < t.sub.size()),
       "an extractvalue past the aggregate");
  const Type r = t.kind == TypeKind::Array ? t.sub[0] : t.sub[index];
  return emit(Op::Extract, r, {aggregate, index}, 0, 0, r);
}

Value Module::atomic_rmw(Rmw op, Value ptr, Value v) {
  const TypeInfo& p = types_[type_of(ptr)];
  need(p.kind == TypeKind::Pointer && p.sub[0] == type_of(v), "an atomic on a pointer to another type");
  return emit(Op::AtomicRmw, type_of(v), {ptr, v}, static_cast<uint8_t>(op), 0, type_of(v));
}

Value Module::cmpxchg(Value ptr, Value expected, Value desired) {
  const Type t = type_of(expected);
  const TypeInfo& p = types_[type_of(ptr)];
  need(p.kind == TypeKind::Pointer && p.sub[0] == t && type_of(desired) == t, "a cmpxchg of other types");
  const Type pair = intern({TypeKind::Struct, 0, {t, i1_}, {}});
  const Value r = emit(Op::CmpXchg, pair, {ptr, expected, desired}, 0, 0, pair);
  return extract(r, 0);
}

void Module::terminate(Op op, std::vector<uint32_t> ops, const std::vector<Block>& succ) {
  const Block from = cur_;
  emit(op, void_, std::move(ops));
  for (Block s : succ) {
    need(s < blocks_.size(), "a branch to a block that is not made");
    blocks_[s].preds.push_back(from);
  }
}

void Module::br(Block to) { terminate(Op::Br, {to}, {to}); }

void Module::cond_br(Value c, Block then_block, Block else_block) {
  need(type_of(c) == i1_, "a branch on a condition not i1");
  terminate(Op::CondBr, {c, then_block, else_block}, {then_block, else_block});
}

void Module::switch_(Value v, Block default_block, const std::vector<std::pair<uint32_t, Block>>& cases) {
  need(type_of(v) == i32_, "a switch on a value not i32");
  std::vector<uint32_t> ops{v, default_block};
  std::vector<Block> succ{default_block};
  for (const auto& [k, b] : cases) {
    ops.push_back(u32(k));
    ops.push_back(b);
    succ.push_back(b);
  }
  terminate(Op::Switch, std::move(ops), succ);
}

void Module::ret() { terminate(Op::Ret, {}, {}); }
void Module::unreachable() { terminate(Op::Unreachable, {}, {}); }

uint32_t Module::variable(Type t) {
  var_types_.push_back(t);
  return static_cast<uint32_t>(var_types_.size() - 1);
}

Value Module::read(uint32_t var) { return emit(Op::Read, var_types_[var], {}, 0, var, var_types_[var]); }

void Module::write(uint32_t var, Value v) {
  if (type_of(v) != var_types_[var]) fail("a variable written with a value of another type");
  emit(Op::Write, void_, {v}, 0, var, var_types_[var]);
}

Value Module::resolve(Value v) const {
  while (v < forward_.size() && forward_[v] != kNone) v = forward_[v];
  return v;
}

// ---- DXIL intrinsics ----

namespace {
const char* overload_name(const TypeInfo& t) {
  switch (t.kind) {
    case TypeKind::Half: return "f16";
    case TypeKind::Float: return "f32";
    case TypeKind::Double: return "f64";
    case TypeKind::Int:
      switch (t.n) {
        case 1: return "i1";
        case 8: return "i8";
        case 16: return "i16";
        case 32: return "i32";
        case 64: return "i64";
        default: break;
      }
      break;
    default: break;
  }
  return "void";
}
}  // namespace

Value Module::dx_call(uint32_t opcode, std::string_view klass, Type overload, Type result, const std::vector<Type>& params,
                      Attr attr, const std::vector<Value>& args) {
  std::string name = "dx.op.";
  name += klass;
  if (overload != kNone) {
    name += '.';
    name += overload_name(types_[overload]);
  }
  std::vector<Type> p{i32_};
  p.insert(p.end(), params.begin(), params.end());
  const Value fn = declare(name, function(result, p), attr);
  std::vector<Value> a{u32(opcode)};
  a.insert(a.end(), args.begin(), args.end());
  return call(fn, a);
}

Value Module::thread_id(uint32_t axis) { return dx_call(93, "threadId", i32_, i32_, {i32_}, Attr::ReadNone, {u32(axis)}); }
Value Module::group_id(uint32_t axis) { return dx_call(94, "groupId", i32_, i32_, {i32_}, Attr::ReadNone, {u32(axis)}); }
Value Module::thread_id_in_group(uint32_t axis) {
  return dx_call(95, "threadIdInGroup", i32_, i32_, {i32_}, Attr::ReadNone, {u32(axis)});
}
Value Module::flat_thread_id_in_group() { return dx_call(96, "flattenedThreadIdInGroup", i32_, i32_, {}, Attr::ReadNone, {}); }

namespace {
// DXIL's resource properties (the annotateHandle constant): kind in bits 0-7, UAV bit 12.
constexpr uint32_t kPropsUav = 1u << 12;
}  // namespace

Value Module::bind_raw_buffer(ResourceClass c, uint32_t space, uint32_t reg) {
  const bool uav = c == ResourceClass::Uav;
  need(uav || c == ResourceClass::Srv, "a raw buffer bound as neither SRV nor UAV");
  const Type st = named_struct(uav ? "struct.RWByteAddressBuffer" : "struct.ByteAddressBuffer", {i32_});
  std::vector<Md>& list = uav ? uavs_ : srvs_;
  const uint32_t id = static_cast<uint32_t>(list.size());
  std::vector<Md> rec{md_u32(id), md_value(undef(pointer(st))), md_string(""), md_u32(space), md_u32(reg), md_u32(1),
                      md_u32(static_cast<uint32_t>(ResourceKind::RawBuffer))};
  if (uav) {
    const Md f = md_value(boolean(false));
    rec.insert(rec.end(), {f, f, f, kNone});
  } else {
    rec.insert(rec.end(), {md_u32(0), kNone});
  }
  list.push_back(md_node(rec));
  flags_ |= kFlagRawBuffers;
  const Value bind = aggregate(res_bind_, {u32(reg), u32(reg), u32(space), u8(static_cast<uint8_t>(c))});
  const Value h = dx_call(217, "createHandleFromBinding", kNone, handle_, {res_bind_, i32_, i1_}, Attr::ReadNone, {bind, u32(reg), boolean(false)});
  const Value props = aggregate(res_props_, {u32(static_cast<uint32_t>(ResourceKind::RawBuffer) | (uav ? kPropsUav : 0u)), u32(0)});
  return dx_call(216, "annotateHandle", kNone, handle_, {handle_, res_props_}, Attr::ReadNone, {h, props});
}

Value Module::bind_cbuffer(uint32_t space, uint32_t reg, uint32_t bytes) {
  const uint32_t rows = (bytes + 15) / 16;
  const Type st = named_struct("struct.am.cbuffer." + std::to_string(rows), {array(vector(i32_, 4), rows)});
  const uint32_t id = static_cast<uint32_t>(cbuffers_.size());
  cbuffers_.push_back(md_node({md_u32(id), md_value(undef(pointer(st))), md_string(""), md_u32(space), md_u32(reg), md_u32(1),
                               md_u32(rows * 16), kNone}));
  const Value bind = aggregate(res_bind_, {u32(reg), u32(reg), u32(space), u8(static_cast<uint8_t>(ResourceClass::CBuffer))});
  const Value h = dx_call(217, "createHandleFromBinding", kNone, handle_, {res_bind_, i32_, i1_}, Attr::ReadNone, {bind, u32(reg), boolean(false)});
  const Value props = aggregate(res_props_, {u32(static_cast<uint32_t>(ResourceKind::CBuffer)), u32(rows * 16)});
  return dx_call(216, "annotateHandle", kNone, handle_, {handle_, res_props_}, Attr::ReadNone, {h, props});
}

Value Module::heap_raw_buffer(Value index, bool uav, bool non_uniform) {
  flags_ |= kFlagRawBuffers;
  return heap_handle(index, false, non_uniform, static_cast<uint32_t>(ResourceKind::RawBuffer) | (uav ? kPropsUav : 0u), 0);
}

Value Module::heap_handle(Value index, bool sampler_heap, bool non_uniform, uint32_t props0, uint32_t props1) {
  flags_ |= sampler_heap ? kFlagHeapSamplers : kFlagHeapResources;
  const Value h = dx_call(218, "createHandleFromHeap", kNone, handle_, {i32_, i1_, i1_}, Attr::ReadNone,
                          {index, boolean(sampler_heap), boolean(non_uniform)});
  const Value props = aggregate(res_props_, {u32(props0), u32(props1)});
  return dx_call(216, "annotateHandle", kNone, handle_, {handle_, res_props_}, Attr::ReadNone, {h, props});
}

Value Module::raw_load(Type t, Value handle, Value offset, uint32_t mask, uint32_t align) {
  const std::string n = std::string("dx.types.ResRet.") + overload_name(types_[t]);
  const Type rr = named_struct(n, {t, t, t, t, i32_});
  return dx_call(139, "rawBufferLoad", t, rr, {handle_, i32_, i32_, i8_, i32_}, Attr::ReadOnly,
                 {handle, offset, undef(i32_), u8(static_cast<uint8_t>(mask)), u32(align)});
}

void Module::raw_store(Value handle, Value offset, const std::vector<Value>& v, uint32_t align) {
  need(!v.empty() && v.size() <= 4, "a raw store of no values or more than four");
  const Type t = type_of(v[0]);
  std::vector<Value> args{handle, offset, undef(i32_)};
  for (size_t k = 0; k < 4; ++k) args.push_back(k < v.size() ? v[k] : undef(t));
  args.push_back(u8(static_cast<uint8_t>((1u << v.size()) - 1u)));
  args.push_back(u32(align));
  dx_call(140, "rawBufferStore", t, void_, {handle_, i32_, i32_, t, t, t, t, i8_, i32_}, Attr::None, args);
}

Value Module::cbuffer_row(Value handle, Value row) {
  const Type cr = named_struct("dx.types.CBufRet.i32", {i32_, i32_, i32_, i32_});
  return dx_call(59, "cbufferLoadLegacy", i32_, cr, {handle_, i32_}, Attr::ReadOnly, {handle, row});
}

void Module::set_compute(uint32_t x, uint32_t y, uint32_t z, uint32_t wave_size) {
  stage_ = Stage::Compute;
  threads_[0] = x;
  threads_[1] = y;
  threads_[2] = z;
  wave_size_ = wave_size;
}

uint32_t Module::add_input(const SigElement& e) {
  inputs_.push_back(e);
  return static_cast<uint32_t>(inputs_.size() - 1);
}

uint32_t Module::add_output(const SigElement& e) {
  outputs_.push_back(e);
  return static_cast<uint32_t>(outputs_.size() - 1);
}

Value Module::load_input(uint32_t id, uint8_t col, bool is_float) {
  const Type t = is_float ? f32_ : i32_;
  return dx_call(4, "loadInput", t, t, {i32_, i32_, i8_, i32_}, Attr::ReadNone, {u32(id), u32(0), u8(col), undef(i32_)});
}

void Module::store_output(uint32_t id, uint8_t col, Value v) {
  const Type t = type_of(v);
  dx_call(5, "storeOutput", t, void_, {i32_, i32_, i8_, t}, Attr::None, {u32(id), u32(0), u8(col), v});
}

void Module::set_mesh(uint32_t x, uint32_t y, uint32_t z, uint32_t max_vertices, uint32_t max_primitives, uint32_t topology) {
  stage_ = Stage::Mesh;
  threads_[0] = x;
  threads_[1] = y;
  threads_[2] = z;
  mesh_vertices_ = max_vertices;
  mesh_primitives_ = max_primitives;
  mesh_topology_ = topology;
}

void Module::store_vertex_output(uint32_t id, uint8_t col, Value v, Value vertex) {
  const Type t = type_of(v);
  dx_call(171, "storeVertexOutput", t, void_, {i32_, i32_, i8_, t, i32_}, Attr::None, {u32(id), u32(0), u8(col), v, vertex});
}

Value Module::attribute_at_vertex(uint32_t id, uint8_t col, uint8_t vertex) {
  return dx_call(137, "attributeAtVertex", f32_, f32_, {i32_, i32_, i8_, i8_}, Attr::ReadNone, {u32(id), u32(0), u8(col), u8(vertex)});
}

// ---- finishing: SSA, checks, metadata ----

// Reachable blocks in reverse postorder, and their predecessors (from reachable blocks only).
bool Module::compute_order(std::string& error) {
  // Reachable blocks in reverse postorder (an explicit stack: programs have 100,000s of blocks).
  const size_t nb = blocks_.size();
  std::vector<uint8_t> seen(nb, 0);
  // Successors are visited first to last, so a branch's first target follows it (as compilers
  // lay blocks out: a loop's body before its exit).
  struct Visit {
    Block b;
    size_t next;
    std::vector<Block> succ;
  };
  std::vector<Visit> stack;
  std::vector<Block> post;
  const auto successors = [&](Block b) {
    std::vector<Block> s;
    if (!terminated(b)) return s;
    each_successor(insts_[blocks_[b].insts.back()], [&](uint32_t x) { s.push_back(x); });
    return s;
  };
  seen[0] = 1;
  stack.push_back({0, 0, successors(0)});
  while (!stack.empty()) {
    Visit& v = stack.back();
    if (v.next == v.succ.size()) {
      post.push_back(v.b);
      stack.pop_back();
      continue;
    }
    const Block s = v.succ[v.next++];
    if (!seen[s]) {
      seen[s] = 1;
      stack.push_back({s, 0, successors(s)});
    }
  }
  order_.assign(post.rbegin(), post.rend());
  for (Block b : order_) {
    if (!terminated(b)) {
      error = "block " + std::to_string(b) + " has no terminator";
      return false;
    }
  }
  // Predecessors from reachable blocks only.
  for (auto& b : blocks_) b.preds.clear();
  for (Block b : order_) each_successor(insts_[blocks_[b].insts.back()], [&](uint32_t s) { blocks_[s].preds.push_back(b); });
  return true;
}

bool Module::finish(std::string& error) {
  if (finished_) return true;
  if (!compute_order(error)) return false;
  build_ssa();
  // AM_DXIL_NO_OPT=1: the module as made (to compare what the optimizer does).
  if (!GetEnvironmentVariableA("AM_DXIL_NO_OPT", nullptr, 0)) optimize();
  // Declarations and globals something left uses (an instruction dropped may have been their last).
  for (auto& f : functions_) f.used = f.body;
  for (auto& g : globals_) g.used = false;
  for (Block b : order_) {
    for (const auto* list : {&blocks_[b].phis, &blocks_[b].insts}) {
      for (uint32_t ii : *list) {
        const Inst& in = insts_[ii];
        if (in.op == Op::Read || in.op == Op::Write || (in.result != kNone && resolve(in.result) != in.result)) continue;
        const bool blocks_in_ops = in.op == Op::Br || in.op == Op::CondBr || in.op == Op::Switch || in.op == Op::Phi;
        for (size_t k = 0; k < in.ops.size(); ++k) {
          // Blocks are not values: a br has only its block; a conditional branch or switch, its
          // condition first; a phi, value and block pairs.
          if (blocks_in_ops && (in.op == Op::Br || (in.op == Op::Phi ? (k & 1) != 0 : k > 0))) continue;
          if (in.op == Op::Extract && k > 0) break;
          const Value v = resolve(in.ops[k]);
          if (v >= values_.size()) continue;
          if (values_[v].kind == ValueKind::Function) {
            functions_[values_[v].bits].used = true;
          }
          if (values_[v].kind == ValueKind::Global) globals_[values_[v].bits].used = true;
        }
      }
    }
  }
  // Shader flags from what the program does; the signatures' usage masks from its loads and
  // stores (attributeAtVertex reads count for no mask, as DXC has it).
  input_masks_.assign(inputs_.size(), 0);
  output_masks_.assign(outputs_.size(), 0);
  for (Block b : order_) {
    for (uint32_t ii : blocks_[b].insts) {
      const Inst& in = insts_[ii];
      if (in.op == Op::Read || in.op == Op::Write) continue;
      if (in.result != kNone && resolve(in.result) != in.result) continue;  // folded or dead: not written
      if (in.result != kNone) {
        const Type t = values_[in.result].type;
        if (t == i64_) flags_ |= kFlagInt64;
        if (t == f64_) flags_ |= kFlagDouble;
        // Double division and conversions between doubles and integers need DXIL's double
        // extensions (as DXC decides it).
        if (in.op == Op::Bin && t == f64_ && static_cast<Bin>(in.code) == Bin::SDiv) flags_ |= kFlagDoubleExtensions;
        if (in.op == Op::Cast) {
          const Cast c = static_cast<Cast>(in.code);
          if ((c == Cast::UiToFp || c == Cast::SiToFp) && t == f64_) flags_ |= kFlagDoubleExtensions;
          if ((c == Cast::FpToUi || c == Cast::FpToSi) && values_[in.ops[0]].type == f64_) flags_ |= kFlagDoubleExtensions;
        }
      }
      if (in.op == Op::Call) {
        const std::string& n = functions_[values_[in.ops[0]].bits].name;
        uint64_t opcode = 0;
        if (n == "dx.op.tertiary.f64" && constant_int(in.ops[1], opcode) && opcode == 47) flags_ |= kFlagDoubleExtensions;  // fma
        // A sample's clamp (its last operand) that is neither undef nor zero: tiled resources.
        const auto starts = [&](std::string_view p) { return std::string_view(n).substr(0, p.size()) == p; };
        if (starts("dx.op.sampleGrad.") || starts("dx.op.sample.") ||
            starts("dx.op.sampleBias.") || starts("dx.op.sampleCmp.")) {
          const Value clamp = resolve(in.ops.back());
          if (!(is_constant(clamp) && values_[clamp].bits == 0)) flags_ |= kFlagTiledResources;
        }
        if (starts("dx.op.loadInput.") || starts("dx.op.storeOutput.") || starts("dx.op.storeVertexOutput.")) {
          const bool load = starts("dx.op.loadInput.");
          uint64_t id = 0, col = 0;
          std::vector<uint8_t>& masks = load ? input_masks_ : output_masks_;
          if (constant_int(in.ops[2], id) && constant_int(in.ops[4], col) && id < masks.size() && col < 8) {
            masks[id] |= static_cast<uint8_t>(1u << col);
            if (load && inputs_[id].kind == 28) flags_ |= kFlagBarycentrics;
          }
        }
        if (starts("dx.op.attributeAtVertex.")) flags_ |= kFlagBarycentrics;
        if (n.compare(0, 10, "dx.op.wave") == 0 || n.compare(0, 10, "dx.op.quad") == 0) flags_ |= kFlagWaveOps;
        if (n == "dx.op.createHandleFromHeap") {
          uint64_t sampler = 0;
          if (constant_int(in.ops[3], sampler) && sampler) flags_ |= kFlagHeapSamplers;
        }
      }
    }
  }
  if (stage_ != Stage::Compute && stage_ != Stage::Pixel && !uavs_.empty()) flags_ |= kFlagUavsEveryStage;
  entry_metadata();
  finished_ = true;
  return true;
}

void Module::entry_metadata() {
  // Drivers read the producer's name: AMD's compiler crashes on a module without one.
  named_metadata("llvm.ident", {md_node({md_string(producer_)})});
  named_metadata("dx.version", {md_node({md_u32(1), md_u32(6)})});
  // Validator 1.6: the first for shader model 6.6, so any newer validator takes the module.
  named_metadata("dx.valver", {md_node({md_u32(1), md_u32(6)})});
  const char* kind = stage_ == Stage::Compute ? "cs" : stage_ == Stage::Vertex ? "vs" : stage_ == Stage::Pixel ? "ps" : "ms";
  named_metadata("dx.shaderModel", {md_node({md_string(kind), md_u32(6), md_u32(6)})});
  Md resources = kNone;
  if (!srvs_.empty() || !uavs_.empty() || !cbuffers_.empty()) {
    const auto list = [&](const std::vector<Md>& l) { return l.empty() ? kNone : md_node(l); };
    resources = md_node({list(srvs_), list(uavs_), list(cbuffers_), kNone});
    named_metadata("dx.resources", {resources});
  }
  flags_md_ = md_value(u64(flags_));
  std::vector<Md> props{md_u32(0), flags_md_};
  if (stage_ == Stage::Compute) {
    props.push_back(md_u32(4));
    props.push_back(md_node({md_u32(threads_[0]), md_u32(threads_[1]), md_u32(threads_[2])}));
  }
  if (stage_ == Stage::Mesh) {
    // The mesh state: {group, most vertices, most primitives, topology, payload bytes}.
    props.push_back(md_u32(9));
    props.push_back(md_node({md_node({md_u32(threads_[0]), md_u32(threads_[1]), md_u32(threads_[2])}), md_u32(mesh_vertices_),
                             md_u32(mesh_primitives_), md_u32(mesh_topology_), md_u32(0)}));
  }
  if (wave_size_) {
    props.push_back(md_u32(11));
    props.push_back(md_node({md_u32(wave_size_)}));
  }
  // Signatures: {inputs, outputs, patch constants}; each element {id, name, component type,
  // semantic kind, {indices}, interpolation, rows, columns, start row, start column, {3: mask}}.
  Md signatures = kNone;
  if (!inputs_.empty() || !outputs_.empty()) {
    const auto i8md = [&](int v) { return md_value(u8(static_cast<uint8_t>(v))); };
    const auto list = [&](const std::vector<SigElement>& els, const std::vector<uint8_t>& masks) {
      if (els.empty()) return kNone;
      std::vector<Md> nodes;
      for (size_t k = 0; k < els.size(); ++k) {
        const SigElement& e = els[k];
        const Md ext = masks[k] ? md_node({md_u32(3), md_u32(masks[k])}) : kNone;
        nodes.push_back(md_node({md_u32(static_cast<uint32_t>(k)), md_string(e.name), i8md(e.comp), i8md(e.kind),
                                 md_node({md_u32(e.index)}), i8md(e.interp), md_u32(e.rows), i8md(e.cols),
                                 md_u32(static_cast<uint32_t>(e.row)), i8md(e.col), ext}));
      }
      return md_node(nodes);
    };
    signatures = md_node({list(inputs_, input_masks_), list(outputs_, output_masks_), kNone});
  }
  const Md entry = md_node({md_value(main_), md_string("main"), signatures, resources, md_node(props)});
  named_metadata("dx.entryPoints", {entry});
}

// Branches the driver need not see: a small triangle or diamond whose arms only compute (no
// stores, calls with effects, wave or quad operations, derivatives, or loads from memory a bad
// index could miss) runs both arms in the block that branched, the join's phis becoming selects;
// a block with one successor that has no other predecessor is merged with it. What DXC's
// SimplifyCFG does for the programs it compiles; every block costs the driver's compiler.
void Module::simplify_cfg() {
  constexpr size_t kSpeculate = 48;  // instructions both arms may hold together
  const auto term = [&](Block x) -> Inst& { return insts_[blocks_[x].insts.back()]; };
  const auto live = [&](const Inst& in) { return in.result == kNone || resolve(in.result) == in.result; };
  const auto only_pred = [&](Block x, Block p) { return blocks_[x].preds.size() == 1 && blocks_[x].preds[0] == p; };
  const auto br_to = [&](Block x, Block& to) {
    const Inst& t = term(x);
    if (t.op != Op::Br) return false;
    to = t.ops[0];
    return true;
  };
  const auto has_live_phi = [&](Block x) {
    for (uint32_t pi : blocks_[x].phis) {
      if (live(insts_[pi])) return true;
    }
    return false;
  };
  const auto incoming = [&](const Inst& phi, Block from) -> Value {
    for (size_t k = 0; k + 1 < phi.ops.size(); k += 2) {
      if (phi.ops[k + 1] == from) return phi.ops[k];
    }
    return kNone;
  };
  // The cost of running a block's body unconditionally, or kNone if it must not be.
  const auto spec_cost = [&](Block x) -> size_t {
    size_t cost = 0;
    const auto& list = blocks_[x].insts;
    for (size_t k = 0; k + 1 < list.size(); ++k) {
      const Inst& in = insts_[list[k]];
      if (in.op == Op::Read || in.op == Op::Write || !live(in)) continue;
      switch (in.op) {
        case Op::Cast:
        case Op::Cmp:
        case Op::Select:
        case Op::Gep:
        case Op::Extract: break;
        case Op::Bin: {
          const Bin op = static_cast<Bin>(in.code);
          uint64_t d = 0;
          const bool division = op == Bin::UDiv || op == Bin::SDiv || op == Bin::URem || op == Bin::SRem;
          if (division && is_int(values_[in.result].type) && !(constant_int(in.ops[1], d) && d != 0)) return kNone;
          break;
        }
        case Op::Call: {
          if (in.result == kNone) return kNone;
          const FunctionInfo& f = functions_[values_[in.ops[0]].bits];
          if (f.attr != Attr::ReadNone && f.attr != Attr::ReadOnly) return kNone;
          const std::string_view n(f.name);
          if (n.substr(0, 10) == "dx.op.wave" || n.substr(0, 10) == "dx.op.quad" || n.substr(0, 18) == "dx.op.isHelperLane") return kNone;
          uint64_t opcode = 0;
          if (constant_int(in.ops[1], opcode) && opcode >= 83 && opcode <= 86) return kNone;  // derivatives
          break;
        }
        default: return kNone;
      }
      ++cost;
    }
    return cost;
  };
  const auto move_body = [&](Block from, Block into) {
    auto& dst = blocks_[into].insts;
    const uint32_t t = dst.back();
    dst.pop_back();
    const auto& src = blocks_[from].insts;
    dst.insert(dst.end(), src.begin(), src.end() - 1);
    dst.push_back(t);
    blocks_[from].insts.clear();
    blocks_[from].phis.clear();
  };
  const auto add_select = [&](Block into, Value c, Value a, Value b) {
    const Type t = values_[resolve(a)].type;
    Inst in;
    in.op = Op::Select;
    in.type = t;
    in.ops = {c, a, b};
    const uint32_t index = static_cast<uint32_t>(insts_.size());
    in.result = add_value(ValueKind::Inst, t, index);
    insts_.push_back(std::move(in));
    auto& list = blocks_[into].insts;
    list.insert(list.end() - 1, index);
    forward_.resize(values_.size(), kNone);
    return insts_[index].result;
  };
  const auto fwd = [&](Value from, Value to) {
    forward_.resize(values_.size(), kNone);
    forward_[from] = to;
  };
  std::string error;
  for (int sweep = 0; sweep < 64; ++sweep) {
    bool changed = false;
    std::vector<uint8_t> touched(blocks_.size(), 0);
    const auto free = [&](std::initializer_list<Block> bs) {
      for (Block x : bs) {
        if (touched[x]) return false;
      }
      return true;
    };
    const auto touch = [&](std::initializer_list<Block> bs) {
      for (Block x : bs) touched[x] = 1;
    };
    const std::vector<Block> order = order_;
    for (const Block b : order) {
      if (blocks_[b].insts.empty() || touched[b]) continue;
      Inst& t = term(b);
      if (t.op == Op::CondBr) {
        const Value c = t.ops[0];
        const Block bt = t.ops[1], bf = t.ops[2];
        if (bt == bf || bt == b || bf == b) continue;
        Block jt = kNone, jf = kNone;
        const bool arm_t = only_pred(bt, b) && br_to(bt, jt) && !has_live_phi(bt);
        const bool arm_f = only_pred(bf, b) && br_to(bf, jf) && !has_live_phi(bf);
        // A diamond: both arms meet at a block with only them before it.
        if (arm_t && arm_f && jt == jf && jt != b && blocks_[jt].preds.size() == 2 && free({b, bt, bf, jt})) {
          const size_t ct = spec_cost(bt), cf = spec_cost(bf);
          if (ct != kNone && cf != kNone && ct + cf <= kSpeculate) {
            move_body(bt, b);
            move_body(bf, b);
            for (uint32_t pi : blocks_[jt].phis) {
              const Inst& p = insts_[pi];
              if (!live(p)) continue;
              fwd(p.result, add_select(b, c, incoming(p, bt), incoming(p, bf)));
            }
            Inst& nt = term(b);
            nt.op = Op::Br;
            nt.ops = {jt};
            touch({b, bt, bf, jt});
            changed = true;
            continue;
          }
        }
        // A triangle: one arm goes on to the other target.
        for (int side = 0; side < 2; ++side) {
          const Block arm = side == 0 ? bt : bf, other = side == 0 ? bf : bt;
          const bool ok = side == 0 ? arm_t && jt == other : arm_f && jf == other;
          if (!ok || blocks_[other].preds.size() != 2 || !free({b, arm, other})) continue;
          const size_t cost = spec_cost(arm);
          if (cost == kNone || cost > kSpeculate) continue;
          move_body(arm, b);
          for (uint32_t pi : blocks_[other].phis) {
            const Inst& p = insts_[pi];
            if (!live(p)) continue;
            const Value va = incoming(p, arm), vb = incoming(p, b);
            fwd(p.result, side == 0 ? add_select(b, c, va, vb) : add_select(b, c, vb, va));
          }
          Inst& nt = term(b);
          nt.op = Op::Br;
          nt.ops = {other};
          touch({b, arm, other});
          changed = true;
          break;
        }
        continue;
      }
      // A chain: b goes only to j, and j is reached only from b.
      Block j = kNone;
      if (br_to(b, j) && j != b && j != order_[0] && only_pred(j, b) && free({b, j})) {
        for (uint32_t pi : blocks_[j].phis) {
          const Inst& p = insts_[pi];
          if (live(p)) fwd(p.result, incoming(p, b));
        }
        std::vector<Block> succ;
        each_successor(term(j), [&](uint32_t x) { succ.push_back(x); });
        auto& dst = blocks_[b].insts;
        dst.pop_back();
        dst.insert(dst.end(), blocks_[j].insts.begin(), blocks_[j].insts.end());
        blocks_[j].insts.clear();
        blocks_[j].phis.clear();
        for (const Block x : succ) {
          for (uint32_t pi : blocks_[x].phis) {
            Inst& p = insts_[pi];
            for (size_t k = 1; k < p.ops.size(); k += 2) {
              if (p.ops[k] == j) p.ops[k] = b;
            }
          }
          touched[x] = 1;
        }
        touch({b, j});
        changed = true;
      }
    }
    if (!changed) break;
    compute_order(error);
  }
}

// ---- optimizing: folding and dead code, after SSA ----

// A conditional branch on a constant becomes a jump (the other successor's phis lose this
// block's entry; a block nothing reaches then drops out of the order).
void Module::fold_constant_branches() {
  bool changed = false;
  for (Block b : order_) {
    Inst& t = insts_[blocks_[b].insts.back()];
    uint64_t k = 0;
    if (t.op != Op::CondBr || !constant_int(t.ops[0], k)) continue;
    const Block keep = k ? t.ops[1] : t.ops[2], drop = k ? t.ops[2] : t.ops[1];
    // One edge goes (when both lead to one block, its phis keep one entry of the two).
    for (uint32_t pi : blocks_[drop].phis) {
      Inst& p = insts_[pi];
      for (size_t j = 0; j + 1 < p.ops.size(); j += 2) {
        if (p.ops[j + 1] == b) {
          p.ops.erase(p.ops.begin() + static_cast<std::ptrdiff_t>(j), p.ops.begin() + static_cast<std::ptrdiff_t>(j + 2));
          break;
        }
      }
    }
    t.op = Op::Br;
    t.ops = {keep};
    changed = true;
  }
  if (!changed) return;
  std::string error;
  compute_order(error);
  // Blocks nothing reaches now were predecessors of others: their phis keep one entry per
  // edge that is left.
  std::unordered_map<Block, uint32_t> edges;
  for (Block b : order_) {
    if (blocks_[b].phis.empty()) continue;
    edges.clear();
    for (Block p : blocks_[b].preds) ++edges[p];
    for (uint32_t pi : blocks_[b].phis) {
      Inst& p = insts_[pi];
      std::unordered_map<Block, uint32_t> left = edges;
      std::vector<uint32_t> kept;
      for (size_t j = 0; j + 1 < p.ops.size(); j += 2) {
        auto it = left.find(p.ops[j + 1]);
        if (it == left.end() || it->second == 0) continue;
        --it->second;
        kept.push_back(p.ops[j]);
        kept.push_back(p.ops[j + 1]);
      }
      p.ops = std::move(kept);
    }
  }
}

// Blocks that only jump on (no live phis, a lone br) go: their predecessors branch to the target,
// whose phis take the value that came through the block from each of them (not done where a
// predecessor already reaches the target directly with another value). The goto form and
// inlined helpers leave many (3db4: 10,084 of 25,210 blocks), which LLVM's SimplifyCFG removes
// for DXC; AMD's compiler otherwise has them all to structure.
void Module::remove_empty_blocks() {
  const auto live_phi = [&](Block x) {
    for (uint32_t pi : blocks_[x].phis) {
      if (resolve(insts_[pi].result) == insts_[pi].result) return true;
    }
    return false;
  };
  const auto incoming = [&](const Inst& phi, Block from) -> Value {
    for (size_t k = 0; k + 1 < phi.ops.size(); k += 2) {
      if (phi.ops[k + 1] == from) return resolve(phi.ops[k]);
    }
    return kNone;
  };
  bool changed = false;
  for (Block b : order_) {
    if (b == order_[0] || blocks_[b].insts.size() != 1 || blocks_[b].preds.empty() || live_phi(b)) continue;
    Inst& t = insts_[blocks_[b].insts.back()];
    if (t.op != Op::Br) continue;
    const Block to = t.ops[0];
    if (to == b) continue;
    // The target's phis: each predecessor of b must give the value b gave (or not reach the target already).
    bool ok = true;
    for (uint32_t pi : blocks_[to].phis) {
      const Inst& p = insts_[pi];
      if (resolve(p.result) != p.result) continue;
      const Value via = incoming(p, b);
      for (Block q : blocks_[b].preds) {
        const Value direct = incoming(p, q);
        if (direct != kNone && direct != via) ok = false;
      }
    }
    if (!ok) continue;
    // Redirect the predecessors' branches.
    for (Block q : blocks_[b].preds) {
      Inst& qt = insts_[blocks_[q].insts.back()];
      switch (qt.op) {
        case Op::Br:
          if (qt.ops[0] == b) qt.ops[0] = to;
          break;
        case Op::CondBr:
          if (qt.ops[1] == b) qt.ops[1] = to;
          if (qt.ops[2] == b) qt.ops[2] = to;
          break;
        case Op::Switch:
          if (qt.ops[1] == b) qt.ops[1] = to;
          for (size_t k = 3; k < qt.ops.size(); k += 2) {
            if (qt.ops[k] == b) qt.ops[k] = to;
          }
          break;
        default: break;
      }
    }
    // The target's phis: b's entry becomes one per edge into b.
    for (uint32_t pi : blocks_[to].phis) {
      Inst& p = insts_[pi];
      const Value via = incoming(p, b);
      if (via == kNone) continue;
      for (size_t k = 0; k + 1 < p.ops.size(); k += 2) {
        if (p.ops[k + 1] == b) {
          p.ops.erase(p.ops.begin() + static_cast<std::ptrdiff_t>(k), p.ops.begin() + static_cast<std::ptrdiff_t>(k + 2));
          break;
        }
      }
      for (Block q : blocks_[b].preds) {
        p.ops.push_back(via);
        p.ops.push_back(q);
      }
    }
    // Predecessor lists, kept current for the blocks after this one.
    auto& tp = blocks_[to].preds;
    for (size_t k = 0; k < tp.size(); ++k) {
      if (tp[k] == b) {
        tp.erase(tp.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
    }
    tp.insert(tp.end(), blocks_[b].preds.begin(), blocks_[b].preds.end());
    blocks_[b].preds.clear();
    changed = true;
  }
  if (changed) {
    std::string error;
    compute_order(error);
  }
}

// Immediate dominators of the reachable blocks (Cooper, Harvey and Kennedy), by block.
std::vector<Block> Module::idoms() const {
  const size_t nb = blocks_.size();
  std::vector<uint32_t> rpo(nb, kNone);
  for (size_t i = 0; i < order_.size(); ++i) rpo[order_[i]] = static_cast<uint32_t>(i);
  std::vector<Block> idom(nb, kNone);
  if (order_.empty()) return idom;
  idom[order_[0]] = order_[0];
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < order_.size(); ++i) {
      const Block b = order_[i];
      Block nd = kNone;
      for (Block p : blocks_[b].preds) {
        if (idom[p] == kNone) continue;
        if (nd == kNone) {
          nd = p;
          continue;
        }
        Block x = p, y = nd;
        while (x != y) {
          while (rpo[x] > rpo[y]) x = idom[x];
          while (rpo[y] > rpo[x]) y = idom[y];
        }
        nd = x;
      }
      if (idom[b] != nd) {
        idom[b] = nd;
        changed = true;
      }
    }
  }
  return idom;
}

// Common subexpressions (what DXC's GVN does): an instruction with no effect that computes
// what one dominating it already computed (the same operation, flags, type and operands; the
// operands of commutative ones in either order) becomes that one. Without it a translated
// program made a descriptor-heap handle at every load (3db4: 7,080 against DXC's 429) and
// repeated its address arithmetic, and AMD's compiler passed 10 GB in 18 s on it. Loads,
// atomics and wave operations are not merged (memory and the wave can change between them).
void Module::cse() {
  forward_.resize(values_.size(), kNone);
  const std::vector<Block> idom = idoms();
  const size_t nb = blocks_.size();
  if (order_.empty()) return;
  std::vector<uint32_t> child_start(nb + 1, 0);
  for (Block b : order_) {
    if (b != order_[0]) ++child_start[idom[b] + 1];
  }
  for (size_t i = 0; i < nb; ++i) child_start[i + 1] += child_start[i];
  std::vector<Block> children(order_.size());
  {
    std::vector<uint32_t> fill(child_start.begin(), child_start.end() - 1);
    for (Block b : order_) {
      if (b != order_[0]) children[fill[idom[b]]++] = b;
    }
  }
  std::unordered_map<std::string, Value> table;
  std::vector<std::pair<std::string, Value>> undo;  // a key and what it held before (kNone: nothing)
  std::string key;
  // Facts the branch into a block gives the blocks it dominates (a block entered from one
  // conditional branch only): the condition's value, and x == k or x != k for a compare of x
  // with a constant. Translated code is full of them: a vector write is select(lane active, new,
  // old) inside a branch that skips the inactive lanes, so the select is the new value there.
  std::unordered_map<Value, bool> known;                       // an i1 value and its value
  std::map<std::pair<Value, Value>, bool> equal;               // (x, constant k): whether x == k
  std::vector<std::pair<Value, int>> known_undo;               // value, previous (-1: none)
  std::vector<std::pair<std::pair<Value, Value>, int>> equal_undo;
  const auto def_of = [&](Value v) -> const Inst* {
    v = resolve(v);
    return values_[v].kind == ValueKind::Inst ? &insts_[values_[v].bits] : nullptr;
  };
  // (0 - zext b), (0 - sext b), zext b, sext b of an i1: b; else kNone (a lane mask spread over a
  // register and its bool).
  const auto mask_bool = [&](Value v) -> Value {
    const Inst* d = def_of(v);
    if (d && d->op == Op::Bin && static_cast<Bin>(d->code) == Bin::Sub) {
      uint64_t k = 1;
      if (!constant_int(d->ops[0], k) || k != 0) return kNone;
      d = def_of(d->ops[1]);
    }
    if (!d || d->op != Op::Cast) return kNone;
    const Cast c = static_cast<Cast>(d->code);
    const Value x = resolve(d->ops[0]);
    return (c == Cast::ZExt || c == Cast::SExt) && values_[x].type == i1_ ? x : kNone;
  };
  const auto set_known = [&](Value c, bool v) {
    c = resolve(c);
    const auto it = known.find(c);
    known_undo.emplace_back(c, it == known.end() ? -1 : static_cast<int>(it->second));
    known[c] = v;
    // A wave test: WaveActiveAllTrue(x) true says x holds in this lane too, WaveActiveAnyTrue(x)
    // false that it does not (the goto form's branches on lane masks).
    if (const Inst* w = def_of(c); w && w->op == Op::Call && w->ops.size() == 3) {
      const std::string& n = functions_[values_[w->ops[0]].bits].name;
      if ((n == "dx.op.waveAllTrue" && v) || (n == "dx.op.waveAnyTrue" && !v)) {
        const Value x = resolve(w->ops[2]);
        const auto kx = known.find(x);
        known_undo.emplace_back(x, kx == known.end() ? -1 : static_cast<int>(kx->second));
        known[x] = v;
        // and what x says in turn (a compare of a mask with zero)
        const Inst* d2 = def_of(x);
        if (d2 && d2->op == Op::Cmp) {
          const Pred p = static_cast<Pred>(d2->code);
          uint64_t k = 0;
          const Value xx = resolve(d2->ops[0]), kv = resolve(d2->ops[1]);
          if ((p == Pred::IEq || p == Pred::INe) && constant_int(kv, k)) {
            const bool eq = p == Pred::IEq ? v : !v;
            const auto key2 = std::make_pair(xx, kv);
            const auto e = equal.find(key2);
            equal_undo.emplace_back(key2, e == equal.end() ? -1 : static_cast<int>(e->second));
            equal[key2] = eq;
          }
        }
      }
    }
    // A compare of x with a constant: x == k (or not) holds too; a mask's bool likewise.
    const Inst* d = def_of(c);
    if (d && d->op == Op::Cmp) {
      const Pred p = static_cast<Pred>(d->code);
      uint64_t k = 0;
      const Value x = resolve(d->ops[0]), kv = resolve(d->ops[1]);
      if ((p == Pred::IEq || p == Pred::INe) && constant_int(kv, k)) {
        const bool eq = p == Pred::IEq ? v : !v;
        const auto key2 = std::make_pair(x, kv);
        const auto e = equal.find(key2);
        equal_undo.emplace_back(key2, e == equal.end() ? -1 : static_cast<int>(e->second));
        equal[key2] = eq;
        // x is a mask (0 - zext b): x == 0 says b is false, x != 0 that it is true.
        if (const Value b = mask_bool(x); b != kNone && k == 0) {
          const auto kb = known.find(b);
          known_undo.emplace_back(b, kb == known.end() ? -1 : static_cast<int>(kb->second));
          known[b] = !eq;
        }
      }
    }
  };
  // What an instruction is, from the facts: kNone if they say nothing.
  const auto from_facts = [&](const Inst& in) -> Value {
    const auto is_known = [&](Value v, bool& out) {
      const auto it = known.find(resolve(v));
      if (it == known.end()) return false;
      out = it->second;
      return true;
    };
    bool kc = false;
    switch (in.op) {
      case Op::Select: {
        uint64_t k = 0;
        if (constant_int(in.ops[0], k)) return k ? in.ops[1] : in.ops[2];
        if (is_known(in.ops[0], kc)) return kc ? in.ops[1] : in.ops[2];
        break;
      }
      case Op::Cmp: {
        const Pred p = static_cast<Pred>(in.code);
        if (p != Pred::IEq && p != Pred::INe) break;
        const Value x = resolve(in.ops[0]), kv = resolve(in.ops[1]);
        uint64_t k = 0;
        if (!constant_int(kv, k)) break;
        const auto e = equal.find(std::make_pair(x, kv));
        if (e != equal.end()) return boolean(p == Pred::IEq ? e->second : !e->second);
        // (0 - zext b) != 0 is b.
        if (const Value b = mask_bool(x); b != kNone && k == 0) {
          if (p == Pred::INe) return b;
          if (is_known(b, kc)) return boolean(!kc);
        }
        break;
      }
      case Op::Bin: {
        if (values_[in.result].type != i1_) break;
        const Bin op = static_cast<Bin>(in.code);
        if (op != Bin::And && op != Bin::Or) break;
        for (int s = 0; s < 2; ++s) {
          if (!is_known(in.ops[s], kc)) continue;
          if (op == Bin::And) return kc ? in.ops[1 - s] : boolean(false);
          return kc ? boolean(true) : in.ops[1 - s];
        }
        break;
      }
      case Op::Cast: {
        const Cast c = static_cast<Cast>(in.code);
        if ((c == Cast::ZExt || c == Cast::SExt) && is_known(in.ops[0], kc)) {
          return const_int(in.type, kc ? (c == Cast::ZExt ? 1u : ~0ull) : 0u);
        }
        break;
      }
      default: break;
    }
    return kNone;
  };
  // Wave and quad operations: the same call on the same values gives the same result within a
  // block (the lanes taking part cannot change inside one), not across blocks. Each first-lane
  // read was a ballot of the lane masks of its own (24ea: 1,990, several a block alike).
  const auto wave_op = [&](const Inst& in) {
    if (in.op != Op::Call) return false;
    const std::string& n = functions_[values_[in.ops[0]].bits].name;
    return n.compare(0, 10, "dx.op.wave") == 0 || n.compare(0, 10, "dx.op.quad") == 0;
  };
  const auto pure = [&](const Inst& in) {
    switch (in.op) {
      case Op::Bin:
      case Op::Cast:
      case Op::Cmp:
      case Op::Select:
      case Op::Gep:
      case Op::Extract: return true;
      case Op::Call: return functions_[values_[in.ops[0]].bits].attr == Attr::ReadNone || wave_op(in);
      default: return false;
    }
  };
  const auto visit = [&](Block b) {
    // Entered only from a conditional branch: its condition is known here and below.
    if (blocks_[b].preds.size() == 1) {
      const Block p = blocks_[b].preds[0];
      const Inst& t = insts_[blocks_[p].insts.back()];
      if (t.op == Op::CondBr && t.ops[1] != t.ops[2] && (t.ops[1] == b || t.ops[2] == b)) set_known(t.ops[0], t.ops[1] == b);
    }
    for (uint32_t ii : blocks_[b].insts) {
      const Inst& in = insts_[ii];
      if (in.result == kNone || in.op == Op::Read || in.op == Op::Write || resolve(in.result) != in.result || !pure(in)) continue;
      if (const Value to = from_facts(in); to != kNone && resolve(to) != in.result) {
        forward_.resize(values_.size(), kNone);
        forward_[in.result] = resolve(to);
        continue;
      }
      key.assign(1, static_cast<char>(in.op));
      key += static_cast<char>(in.code);
      key_u32(key, in.aux);
      key_u32(key, in.type);
      key_u32(key, values_[in.result].type);
      if (wave_op(in)) key_u32(key, b);  // within this block only
      std::vector<uint32_t> ops(in.ops.begin(), in.ops.end());
      const size_t values = in.op == Op::Extract ? 1 : ops.size();  // an extract's indices are numbers
      for (size_t k = 0; k < values; ++k) ops[k] = resolve(ops[k]);
      const bool commutative =
          (in.op == Op::Bin && (static_cast<Bin>(in.code) == Bin::Add || static_cast<Bin>(in.code) == Bin::Mul ||
                                static_cast<Bin>(in.code) == Bin::And || static_cast<Bin>(in.code) == Bin::Or ||
                                static_cast<Bin>(in.code) == Bin::Xor)) ||
          (in.op == Op::Cmp && (static_cast<Pred>(in.code) == Pred::IEq || static_cast<Pred>(in.code) == Pred::INe));
      if (commutative && ops.size() == 2 && ops[0] > ops[1]) std::swap(ops[0], ops[1]);
      for (uint32_t o : ops) key_u32(key, o);
      const auto it = table.find(key);
      if (it != table.end()) {
        forward_.resize(values_.size(), kNone);
        forward_[in.result] = it->second;
        continue;
      }
      undo.emplace_back(key, kNone);
      table.emplace(key, in.result);
    }
  };
  struct Frame {
    Block b;
    uint32_t next;
    size_t mark, known_mark, equal_mark;
  };
  std::vector<Frame> frames;
  const auto enter = [&](Block b) {
    const size_t mark = undo.size(), km = known_undo.size(), em = equal_undo.size();
    visit(b);
    frames.push_back({b, child_start[b], mark, km, em});
  };
  enter(order_[0]);
  while (!frames.empty()) {
    Frame& f = frames.back();
    if (f.next < child_start[f.b + 1]) {
      const Block c = children[f.next++];
      enter(c);
      continue;
    }
    while (undo.size() > f.mark) {
      table.erase(undo.back().first);
      undo.pop_back();
    }
    while (known_undo.size() > f.known_mark) {
      if (known_undo.back().second < 0) known.erase(known_undo.back().first);
      else known[known_undo.back().first] = known_undo.back().second != 0;
      known_undo.pop_back();
    }
    while (equal_undo.size() > f.equal_mark) {
      if (equal_undo.back().second < 0) equal.erase(equal_undo.back().first);
      else equal[equal_undo.back().first] = equal_undo.back().second != 0;
      equal_undo.pop_back();
    }
    frames.pop_back();
  }
}

// Values whose operands are constants, or whose operation does nothing, become what they are
// (forwarded: the writer skips an instruction whose value is forwarded); then every instruction
// nothing with an effect depends on goes (forwarded to undef: nothing uses it). The translated
// programs compute much they never use, and convert booleans back and forth: this keeps the
// module small for the driver's compiler.
void Module::optimize() {
  forward_.resize(values_.size(), kNone);
  const auto def = [&](Value v) -> const Inst* {
    v = resolve(v);
    return values_[v].kind == ValueKind::Inst ? &insts_[values_[v].bits] : nullptr;
  };
  const auto fwd = [&](Value from, Value to) {
    forward_.resize(values_.size(), kNone);
    forward_[from] = to;
  };
  // A zext or sext of an i1: that i1, else kNone.
  const auto bool_widened = [&](Value v) -> Value {
    const Inst* d = def(v);
    if (!d || d->op != Op::Cast) return kNone;
    const Cast c = static_cast<Cast>(d->code);
    const Value x = resolve(d->ops[0]);
    return (c == Cast::ZExt || c == Cast::SExt) && values_[x].type == i1_ ? x : kNone;
  };
  const auto sext_bits = [](uint64_t k, uint32_t bits) {
    return bits >= 64 ? static_cast<int64_t>(k) : static_cast<int64_t>(k << (64 - bits)) >> (64 - bits);
  };
  // A register pair made 64 bits (the translator's am_u64: (zext hi << 32) | zext lo, either
  // order): lo (i32), the zext of lo and the shifted part's operand (hi's zext, or what it is).
  // DXC sees through taking such a pair apart again (uint(r), uint(r >> 32)); without it a giant
  // kept 20,000 64-bit shifts DXC has none of.
  const auto pair_parts = [&](Value v, Value& lo, Value& lo_ext, Value& hi_ext) {
    const Inst* o = def(v);
    if (!o || o->op != Op::Bin || static_cast<Bin>(o->code) != Bin::Or || values_[resolve(v)].type != i64_) return false;
    for (int s = 0; s < 2; ++s) {
      const Inst* sh = def(o->ops[s]);
      const Inst* z = def(o->ops[1 - s]);
      uint64_t k = 0;
      if (!sh || sh->op != Op::Bin || static_cast<Bin>(sh->code) != Bin::Shl || !constant_int(sh->ops[1], k) || k != 32) continue;
      if (!z || z->op != Op::Cast || static_cast<Cast>(z->code) != Cast::ZExt || values_[resolve(z->ops[0])].type != i32_) continue;
      lo = resolve(z->ops[0]);
      lo_ext = resolve(o->ops[1 - s]);
      hi_ext = resolve(sh->ops[0]);
      return true;
    }
    return false;
  };
  // Folding; then common subexpressions and what branches say (which leaves constants for
  // folding again); then branches on constants become jumps and the CFG is simplified.
  for (int pass = 0; pass < 3; ++pass) {
  if (pass == 1) cse();
  if (pass == 2) {
    fold_constant_branches();
    simplify_cfg();
    remove_empty_blocks();
    simplify_cfg();
  }
  for (int round = 0; round < 16; ++round) {
    bool changed = false;
    for (Block b : order_) {
      for (uint32_t pi : blocks_[b].phis) {
        const Inst& p = insts_[pi];
        if (resolve(p.result) != p.result) continue;
        Value same = kNone;
        bool trivial = true;
        for (size_t k = 0; k < p.ops.size(); k += 2) {
          const Value o = resolve(p.ops[k]);
          if (o == p.result || o == same) continue;
          if (same != kNone) {
            trivial = false;
            break;
          }
          same = o;
        }
        if (trivial && same != kNone) {
          fwd(p.result, same);
          changed = true;
        }
      }
      for (uint32_t ii : blocks_[b].insts) {
        const Inst& in = insts_[ii];
        if (in.result == kNone || in.op == Op::Read || in.op == Op::Write || resolve(in.result) != in.result) continue;
        const Value r = in.result;
        const Type t = values_[r].type;
        Value to = kNone;
        uint64_t ka = 0, kb = 0;
        switch (in.op) {
          case Op::Cast: {
            const Cast c = static_cast<Cast>(in.code);
            const Value x = resolve(in.ops[0]);
            const Type ft = values_[x].type;
            if (is_int(t) && is_int(ft) && constant_int(x, ka)) {
              if (c == Cast::ZExt || c == Cast::Trunc) to = const_int(t, ka);
              else if (c == Cast::SExt) to = const_int(t, static_cast<uint64_t>(sext_bits(ka, types_[ft].n)));
            } else if (c == Cast::Trunc || c == Cast::Bitcast) {
              const Inst* d = def(x);
              Value lo = kNone, lo_ext = kNone, hi_ext = kNone;
              if (d && d->op == Op::Cast) {
                const Cast dc = static_cast<Cast>(d->code);
                const Value inner = resolve(d->ops[0]);
                const bool undoes = c == Cast::Trunc ? (dc == Cast::ZExt || dc == Cast::SExt) : dc == Cast::Bitcast;
                if (undoes && values_[inner].type == t) to = inner;
              } else if (c == Cast::Trunc && t == i32_ && pair_parts(x, lo, lo_ext, hi_ext)) {
                to = lo;  // uint(am_u64(lo, hi))
              }
            }
            break;
          }
          case Op::Cmp: {
            const Pred p = static_cast<Pred>(in.code);
            const Value a = resolve(in.ops[0]), b2 = resolve(in.ops[1]);
            if (static_cast<uint8_t>(p) < static_cast<uint8_t>(Pred::IEq) || !is_int(values_[a].type)) break;
            const uint32_t bits = types_[values_[a].type].n;
            if (constant_int(a, ka) && constant_int(b2, kb)) {
              const int64_t sa = sext_bits(ka, bits), sb = sext_bits(kb, bits);
              bool v = false;
              switch (p) {
                case Pred::IEq: v = ka == kb; break;
                case Pred::INe: v = ka != kb; break;
                case Pred::IUgt: v = ka > kb; break;
                case Pred::IUge: v = ka >= kb; break;
                case Pred::IUlt: v = ka < kb; break;
                case Pred::IUle: v = ka <= kb; break;
                case Pred::ISgt: v = sa > sb; break;
                case Pred::ISge: v = sa >= sb; break;
                case Pred::ISlt: v = sa < sb; break;
                case Pred::ISle: v = sa <= sb; break;
                default: break;
              }
              to = boolean(v);
            } else if (a == b2) {
              to = boolean(p == Pred::IEq || p == Pred::IUge || p == Pred::IUle || p == Pred::ISge || p == Pred::ISle);
            } else if (const Value w = bool_widened(a); w != kNone && constant_int(b2, kb)) {
              // (zext b) != 0, (zext b) > 0, (zext b) == 1: b.
              if (kb == 0 && (p == Pred::INe || p == Pred::IUgt)) to = w;
              if (kb == 1 && p == Pred::IEq && static_cast<Cast>(def(a)->code) == Cast::ZExt) to = w;
            }
            break;
          }
          case Op::Select: {
            const Value c = resolve(in.ops[0]), a = resolve(in.ops[1]), b2 = resolve(in.ops[2]);
            if (a == b2) to = a;
            else if (constant_int(c, ka)) to = ka ? a : b2;
            else if (t == i1_ && constant_int(a, ka) && constant_int(b2, kb) && ka == 1 && kb == 0) to = c;
            break;
          }
          case Op::Bin: {
            if (!is_int(t)) break;
            const Bin op = static_cast<Bin>(in.code);
            const Value a = resolve(in.ops[0]), b2 = resolve(in.ops[1]);
            const bool ca = constant_int(a, ka), cb = constant_int(b2, kb);
            const uint32_t bits = types_[t].n;
            const uint64_t ones = bits >= 64 ? ~0ull : (1ull << bits) - 1;
            if (ca && cb) {
              bool ok = true;
              uint64_t v = 0;
              switch (op) {
                case Bin::Add: v = ka + kb; break;
                case Bin::Sub: v = ka - kb; break;
                case Bin::Mul: v = ka * kb; break;
                case Bin::And: v = ka & kb; break;
                case Bin::Or: v = ka | kb; break;
                case Bin::Xor: v = ka ^ kb; break;
                case Bin::Shl: v = kb < bits ? ka << kb : 0; ok = kb < bits; break;
                case Bin::LShr: v = kb < bits ? ka >> kb : 0; ok = kb < bits; break;
                case Bin::AShr: v = static_cast<uint64_t>(sext_bits(ka, bits) >> kb); ok = kb < bits; break;
                case Bin::UDiv: ok = kb != 0; v = ok ? ka / kb : 0; break;
                case Bin::URem: ok = kb != 0; v = ok ? ka % kb : 0; break;
                default: ok = false; break;
              }
              if (ok) to = const_int(t, v & ones);
              break;
            }
            const bool a0 = ca && ka == 0, b0 = cb && kb == 0, a1 = ca && ka == 1, b1 = cb && kb == 1;
            const bool aall = ca && ka == ones, ball = cb && kb == ones;
            switch (op) {
              case Bin::Add:
              case Bin::Or:
              case Bin::Xor:
                if (b0) to = a;
                else if (a0) to = b2;
                else if (op == Bin::Or && a == b2) to = a;
                else if (op == Bin::Xor && a == b2) to = const_int(t, 0);
                else if (op == Bin::Or && (aall || ball)) to = const_int(t, ones);
                break;
              case Bin::Sub:
                if (b0) to = a;
                else if (a == b2) to = const_int(t, 0);
                break;
              case Bin::Shl:
              case Bin::LShr:
              case Bin::AShr:
                if (b0) to = a;
                else if (a0) to = a;
                else if (op == Bin::LShr && t == i64_ && cb && kb == 32) {
                  // am_u64(lo, hi) >> 32: hi's zext (its top half is zero).
                  Value lo = kNone, lo_ext = kNone, hi_ext = kNone;
                  if (pair_parts(a, lo, lo_ext, hi_ext)) {
                    const Inst* z = def(hi_ext);
                    if (z && z->op == Op::Cast && static_cast<Cast>(z->code) == Cast::ZExt && values_[resolve(z->ops[0])].type == i32_) to = hi_ext;
                  }
                }
                break;
              case Bin::Mul:
                if (b1) to = a;
                else if (a1) to = b2;
                else if (a0 || b0) to = const_int(t, 0);
                break;
              case Bin::And:
                if (a0 || b0) to = const_int(t, 0);
                else if (ball) to = a;
                else if (aall) to = b2;
                else if (a == b2) to = a;
                else if (t == i64_ && cb && kb == 0xFFFFFFFFull) {
                  Value lo = kNone, lo_ext = kNone, hi_ext = kNone;
                  if (pair_parts(a, lo, lo_ext, hi_ext)) to = lo_ext;  // am_u64(lo, hi) & 0xFFFFFFFF
                }
                break;
              case Bin::UDiv:
                if (b1) to = a;
                break;
              default: break;
            }
            break;
          }
          default: break;
        }
        if (to != kNone && to != r) {
          fwd(r, to);
          changed = true;
        }
      }
    }
    if (!changed) break;
  }
  }

  cse();

  // Dead code: from what has an effect (stores, atomics, calls that write or synchronize,
  // terminators), every instruction whose value it uses, transitively.
  std::vector<uint8_t> live(insts_.size(), 0);
  std::vector<uint32_t> work;
  const auto use = [&](Value v) {
    v = resolve(v);
    if (values_[v].kind != ValueKind::Inst) return;
    const uint32_t i = static_cast<uint32_t>(values_[v].bits);
    if (!live[i]) {
      live[i] = 1;
      work.push_back(i);
    }
  };
  const auto has_effect = [&](const Inst& in) {
    switch (in.op) {
      case Op::Store:
      case Op::AtomicRmw:
      case Op::CmpXchg:
      case Op::Br:
      case Op::CondBr:
      case Op::Switch:
      case Op::Ret:
      case Op::Unreachable: return true;
      case Op::Call: {
        const Attr a = functions_[values_[in.ops[0]].bits].attr;
        return in.result == kNone || (a != Attr::ReadNone && a != Attr::ReadOnly);
      }
      default: return false;
    }
  };
  for (Block b : order_) {
    for (uint32_t ii : blocks_[b].insts) {
      const Inst& in = insts_[ii];
      if (in.op == Op::Read || in.op == Op::Write) continue;
      if (has_effect(in)) {
        live[ii] = 1;
        work.push_back(ii);
      }
    }
  }
  while (!work.empty()) {
    const Inst& in = insts_[work.back()];
    work.pop_back();
    switch (in.op) {
      case Op::Phi:
        for (size_t k = 0; k < in.ops.size(); k += 2) use(in.ops[k]);
        break;
      case Op::Br: break;
      case Op::CondBr:
      case Op::Switch: use(in.ops[0]); break;
      case Op::Extract: use(in.ops[0]); break;
      case Op::Call:
        for (size_t k = 1; k < in.ops.size(); ++k) use(in.ops[k]);
        break;
      default:
        for (const uint32_t o : in.ops) use(o);
        break;
    }
  }
  for (Block b : order_) {
    for (const auto* list : {&blocks_[b].phis, &blocks_[b].insts}) {
      for (uint32_t ii : *list) {
        const Inst& in = insts_[ii];
        if (live[ii] || in.result == kNone || in.op == Op::Read || in.op == Op::Write || resolve(in.result) != in.result) continue;
        fwd(in.result, undef(values_[in.result].type));
      }
    }
  }
}

// Registers into SSA values: phis where a register's definitions meet (the iterated dominance
// frontier of its writes), only where it is live; then each read takes the definition that
// reaches it, walking the dominator tree. Explicit stacks throughout: no recursion.
void Module::build_ssa() {
  const size_t nb = blocks_.size(), nv = var_types_.size();
  forward_.assign(values_.size(), kNone);
  if (nv == 0 || order_.empty()) return;
  std::vector<uint32_t> rpo(nb, kNone);
  for (size_t i = 0; i < order_.size(); ++i) rpo[order_[i]] = static_cast<uint32_t>(i);

  // Dominators (Cooper, Harvey and Kennedy's iteration over reverse postorder).
  std::vector<Block> idom(nb, kNone);
  idom[order_[0]] = order_[0];
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < order_.size(); ++i) {
      const Block b = order_[i];
      Block nd = kNone;
      for (Block p : blocks_[b].preds) {
        if (idom[p] == kNone) continue;
        if (nd == kNone) {
          nd = p;
          continue;
        }
        Block x = p, y = nd;
        while (x != y) {
          while (rpo[x] > rpo[y]) x = idom[x];
          while (rpo[y] > rpo[x]) y = idom[y];
        }
        nd = x;
      }
      if (idom[b] != nd) {
        idom[b] = nd;
        changed = true;
      }
    }
  }
  // Dominance frontiers.
  std::vector<std::vector<Block>> df(nb);
  for (Block b : order_) {
    const auto& preds = blocks_[b].preds;
    if (preds.size() < 2) continue;
    for (Block p : preds) {
      for (Block r = p; r != idom[b]; r = idom[r]) {
        if (df[r].empty() || df[r].back() != b) df[r].push_back(b);
      }
    }
  }
  // Per variable: the blocks that write it, and the blocks that read it before writing it.
  std::vector<std::vector<Block>> defs(nv), uses(nv);
  {
    std::vector<Block> wrote(nv, kNone), used(nv, kNone);
    for (Block b : order_) {
      for (uint32_t ii : blocks_[b].insts) {
        const Inst& in = insts_[ii];
        if (in.op == Op::Read) {
          if (wrote[in.aux] != b && used[in.aux] != b) {
            used[in.aux] = b;
            uses[in.aux].push_back(b);
          }
        } else if (in.op == Op::Write) {
          if (wrote[in.aux] != b) {
            wrote[in.aux] = b;
            defs[in.aux].push_back(b);
          }
        }
      }
    }
  }
  // Phis: the iterated frontier of the writes, where the variable is live on entry.
  std::vector<uint32_t> live(nb, kNone), is_def(nb, kNone), has_phi(nb, kNone), queued(nb, kNone);
  std::vector<Block> work;
  for (uint32_t v = 0; v < nv; ++v) {
    if (uses[v].empty() || defs[v].empty()) continue;
    for (Block b : defs[v]) is_def[b] = v;
    work.clear();
    for (Block b : uses[v]) {
      live[b] = v;
      work.push_back(b);
    }
    while (!work.empty()) {
      const Block b = work.back();
      work.pop_back();
      for (Block p : blocks_[b].preds) {
        if (live[p] == v || is_def[p] == v) continue;
        live[p] = v;
        work.push_back(p);
      }
    }
    work.assign(defs[v].begin(), defs[v].end());
    for (Block b : defs[v]) queued[b] = v;
    while (!work.empty()) {
      const Block b = work.back();
      work.pop_back();
      for (Block f : df[b]) {
        if (has_phi[f] == v) continue;
        has_phi[f] = v;
        if (live[f] == v) {
          Inst phi;
          phi.op = Op::Phi;
          phi.aux = v;
          phi.type = var_types_[v];
          phi.result = add_value(ValueKind::Inst, var_types_[v], insts_.size());
          blocks_[f].phis.push_back(add_inst(std::move(phi)));
        }
        if (queued[f] != v) {
          queued[f] = v;
          work.push_back(f);
        }
      }
    }
  }
  forward_.resize(values_.size(), kNone);

  // Renaming down the dominator tree.
  std::vector<uint32_t> child_start(nb + 1, 0);
  for (Block b : order_) {
    if (b != order_[0]) ++child_start[idom[b] + 1];
  }
  for (size_t i = 0; i < nb; ++i) child_start[i + 1] += child_start[i];
  std::vector<Block> children(order_.size());
  {
    std::vector<uint32_t> fill(child_start.begin(), child_start.end() - 1);
    for (Block b : order_) {
      if (b != order_[0]) children[fill[idom[b]]++] = b;
    }
  }
  std::vector<Value> cur(nv, kNone);
  struct Undo {
    uint32_t var;
    Value old;
  };
  std::vector<Undo> undo;
  struct Frame {
    Block b;
    uint32_t next;
    size_t mark;
  };
  std::vector<Frame> frames;
  const auto enter = [&](Block b) {
    const size_t mark = undo.size();
    for (uint32_t pi : blocks_[b].phis) {
      const Inst& p = insts_[pi];
      undo.push_back({p.aux, cur[p.aux]});
      cur[p.aux] = p.result;
    }
    for (uint32_t ii : blocks_[b].insts) {
      Inst& in = insts_[ii];
      if (in.op == Op::Read) {
        // A variable read before any write reads zero, not undef: the validator refuses
        // instructions that read undef (a masked write keeps the register's old value).
        const Value c = cur[in.aux];
        const Value r = in.result;
        const Value u = c != kNone ? c : zero(var_types_[in.aux]);
        forward_.resize(values_.size(), kNone);
        forward_[r] = u;
      } else if (in.op == Op::Write) {
        undo.push_back({in.aux, cur[in.aux]});
        cur[in.aux] = resolve(in.ops[0]);
      }
    }
    each_successor(insts_[blocks_[b].insts.back()], [&](uint32_t s) {
      for (uint32_t pi : blocks_[s].phis) {
        const uint32_t var = insts_[pi].aux;
        const Value c = cur[var] != kNone ? cur[var] : zero(var_types_[var]);
        insts_[pi].ops.push_back(c);
        insts_[pi].ops.push_back(b);
      }
    });
    frames.push_back({b, child_start[b], mark});
  };
  enter(order_[0]);
  while (!frames.empty()) {
    Frame& f = frames.back();
    if (f.next < child_start[f.b + 1]) {
      const Block c = children[f.next++];
      enter(c);
      continue;
    }
    while (undo.size() > f.mark) {
      cur[undo.back().var] = undo.back().old;
      undo.pop_back();
    }
    frames.pop_back();
  }
  forward_.resize(values_.size(), kNone);

  // Trivial phis (one value besides themselves) become that value, until none is left.
  for (bool again = true; again;) {
    again = false;
    for (Block b : order_) {
      for (uint32_t pi : blocks_[b].phis) {
        const Inst& p = insts_[pi];
        if (forward_[p.result] != kNone) continue;
        Value same = kNone;
        bool trivial = true;
        for (size_t k = 0; k < p.ops.size(); k += 2) {
          const Value o = resolve(p.ops[k]);
          if (o == p.result || o == same) continue;
          if (same != kNone) {
            trivial = false;
            break;
          }
          same = o;
        }
        if (!trivial) continue;
        const Value r = p.result;
        const Value to = same != kNone ? same : zero(p.type);
        forward_.resize(values_.size(), kNone);
        forward_[r] = to;
        again = true;
      }
    }
  }
}

void Module::set_flags(uint64_t f) {
  need(finished_, "flags set before finish()");
  flags_ = f;
  md_[flags_md_].value = u64(f);
}

bool validator_flags(std::string_view messages, uint64_t& actual) {
  const size_t at = messages.find("actual=");
  if (at == std::string_view::npos) return false;
  size_t k = at + 7;
  if (k >= messages.size() || messages[k] < '0' || messages[k] > '9') return false;
  uint64_t v = 0;
  while (k < messages.size() && messages[k] >= '0' && messages[k] <= '9') v = v * 10 + static_cast<uint64_t>(messages[k++] - '0');
  actual = v;
  return true;
}

}  // namespace am::shader::dxil
