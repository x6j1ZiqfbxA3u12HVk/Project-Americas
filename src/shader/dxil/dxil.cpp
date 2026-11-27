#include "shader/dxil/dxil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

Value Module::global(std::string_view name, Type t, uint32_t space, uint32_t align) {
  const Value v = add_value(ValueKind::Global, pointer(t, space), globals_.size());
  globals_.push_back({std::string(name), t, space, align, v});
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

Value Module::cmp(Pred p, Value a, Value b) {
  const Type t = type_of(a);
  need(t == type_of(b), "a compare of two types");
  need(static_cast<uint8_t>(p) >= 32 ? is_int(t) : is_float(t), "a compare predicate of the other kind");
  return emit(Op::Cmp, i1_, {a, b}, static_cast<uint8_t>(p), 0, i1_);
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
  flags_ |= kFlagHeapResources | kFlagRawBuffers;
  const Value h = dx_call(218, "createHandleFromHeap", kNone, handle_, {i32_, i1_, i1_}, Attr::ReadNone,
                          {index, boolean(false), boolean(non_uniform)});
  const Value props = aggregate(res_props_, {u32(static_cast<uint32_t>(ResourceKind::RawBuffer) | (uav ? kPropsUav : 0u)), u32(0)});
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

// ---- finishing: SSA, checks, metadata ----

bool Module::finish(std::string& error) {
  if (finished_) return true;
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
  build_ssa();
  // Shader flags from what the program does.
  for (Block b : order_) {
    for (uint32_t ii : blocks_[b].insts) {
      const Inst& in = insts_[ii];
      if (in.op == Op::Read || in.op == Op::Write) continue;
      if (in.result != kNone) {
        const Type t = values_[in.result].type;
        if (t == i64_) flags_ |= kFlagInt64;
        if (t == f64_) flags_ |= kFlagDouble;
      }
      if (in.op == Op::Call) {
        const std::string& n = functions_[values_[in.ops[0]].bits].name;
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
  named_metadata("llvm.ident", {md_node({md_string("Project Americas DXIL writer")})});
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
  if (wave_size_) {
    props.push_back(md_u32(11));
    props.push_back(md_node({md_u32(wave_size_)}));
  }
  const Md entry = md_node({md_value(main_), md_string("main"), kNone, resources, md_node(props)});
  named_metadata("dx.entryPoints", {entry});
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
        const Value c = cur[in.aux];
        const Value r = in.result;
        const Value u = c != kNone ? c : undef(var_types_[in.aux]);
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
        const Value c = cur[var] != kNone ? cur[var] : undef(var_types_[var]);
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
        const Value to = same != kNone ? same : undef(p.type);
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
