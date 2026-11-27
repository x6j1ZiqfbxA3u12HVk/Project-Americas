#pragma once

// Our own DXIL. A Module holds one entry function; write_bitcode() writes it as LLVM 3.7 bitcode
// (the form DXIL is) and DXC's assembler and validator make the signed container from that
// (host::assemble_dxil). Console programs keep their own branches this way: HLSL cannot express a
// control flow graph that does not nest, or nests deeper than DXC's 256 levels.
//
// Building: make blocks (block()), pick one (at()), add instructions to it; every block ends in
// one terminator (br, cond_br, switch_, ret, unreachable). Console registers are variables
// (variable(), read(), write()); finish() makes them SSA values (phis where paths join, pruned to
// where the register is live), so a program is built in any order with no phis by hand.
//
// Values, types, blocks and metadata are indices; operands are checked when they are added (a
// mismatch is a translator bug and aborts with a message rather than writing a bad module).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace am::shader::dxil {

using Type = uint32_t;
using Value = uint32_t;
using Block = uint32_t;
using Md = uint32_t;
constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class TypeKind : uint8_t { Void, Label, Metadata, Int, Half, Float, Double, Pointer, Struct, Array, Vector, Function };

struct TypeInfo {
  TypeKind kind = TypeKind::Void;
  uint32_t n = 0;         // Int: bits; Array, Vector: elements; Pointer: address space
  std::vector<Type> sub;  // Pointer: pointee; Array, Vector: element; Struct: members; Function: result, then parameters
  std::string name;       // a named struct
};

// Attribute sets of declared functions (each also nounwind).
enum class Attr : uint8_t { None, ReadNone, ReadOnly, NoDuplicate };

enum class ValueKind : uint8_t { Global, Function, Int, Float, Null, Undef, Aggregate, Inst };

struct ValueInfo {
  ValueKind kind = ValueKind::Undef;
  Type type = 0;
  // Int, Float: the bits (zero-extended); Global, Function, Aggregate: its index in its table;
  // Inst: its instruction.
  uint64_t bits = 0;
};

enum class Op : uint8_t {
  Bin, Cast, Cmp, Select, Call, Load, Store, Gep, Alloca, Extract, Phi, AtomicRmw, CmpXchg,
  Br, CondBr, Switch, Ret, Unreachable,
  Read, Write,  // variables, until finish()
};

// LLVM's binary operators; floating point types make them fadd, fsub, ...
enum class Bin : uint8_t { Add, Sub, Mul, UDiv, SDiv, URem, SRem, Shl, LShr, AShr, And, Or, Xor };
enum class Cast : uint8_t { Trunc, ZExt, SExt, FpToUi, FpToSi, UiToFp, SiToFp, FpTrunc, FpExt, PtrToInt, IntToPtr, Bitcast };
enum class Pred : uint8_t {
  FFalse = 0, FOeq, FOgt, FOge, FOlt, FOle, FOne, FOrd, FUno, FUeq, FUgt, FUge, FUlt, FUle, FUne, FTrue,
  IEq = 32, INe, IUgt, IUge, IUlt, IUle, ISgt, ISge, ISlt, ISle,
};
enum class Rmw : uint8_t { Xchg, Add, Sub, And, Nand, Or, Xor, Max, Min, UMax, UMin };

// Fast-math flags on floating point operators (LLVM's bits).
constexpr uint32_t kFastMath = 0x1Fu;

struct Inst {
  Op op = Op::Ret;
  uint8_t code = 0;   // Bin, Cast, Pred, Rmw
  uint32_t aux = 0;   // Bin: flags; Load, Store, Alloca: alignment in bytes; Read, Write: the variable
  Type type = 0;      // the result's type (Load: loaded; Alloca: allocated; Gep: the source element)
  Value result = kNone;
  // Operands. Phi: value, block pairs. Br: block. CondBr: condition, then, else. Switch:
  // condition, default, then case value, block pairs. Call: callee, arguments. Extract:
  // aggregate, then indices (numbers). Gep: base, indices.
  std::vector<uint32_t> ops;
};

struct BlockInfo {
  std::vector<uint32_t> phis;   // made by finish()
  std::vector<uint32_t> insts;  // the last is the terminator
  std::vector<Block> preds;     // one per edge (a switch can have several to one block)
};

// Resource kinds and classes as DXIL numbers them.
enum class ResourceKind : uint8_t {
  Texture1D = 1, Texture2D, Texture2DMS, Texture3D, TextureCube, Texture1DArray, Texture2DArray, Texture2DMSArray,
  TextureCubeArray, TypedBuffer, RawBuffer, StructuredBuffer, CBuffer, Sampler,
};
enum class ResourceClass : uint8_t { Srv, Uav, CBuffer, Sampler };

enum class Stage : uint8_t { Compute, Vertex, Pixel, Mesh };

class Module {
 public:
  Module();

  // Types (interned).
  Type void_type() const { return void_; }
  Type i1() const { return i1_; }
  Type i8() const { return i8_; }
  Type i16() { return int_type(16); }
  Type i32() const { return i32_; }
  Type i64() const { return i64_; }
  Type f16() const { return f16_; }
  Type f32() const { return f32_; }
  Type f64() const { return f64_; }
  Type int_type(uint32_t bits);
  Type pointer(Type pointee, uint32_t space = 0);
  Type array(Type element, uint32_t n);
  Type vector(Type element, uint32_t n);
  Type function(Type result, const std::vector<Type>& params);
  Type named_struct(std::string_view name, const std::vector<Type>& members);
  const TypeInfo& type(Type t) const { return types_[t]; }
  const std::vector<TypeInfo>& types() const { return types_; }

  // Constants (interned).
  Value const_int(Type t, uint64_t v);
  Value u32(uint32_t v) { return const_int(i32_, v); }
  Value u64(uint64_t v) { return const_int(i64_, v); }
  Value u8(uint8_t v) { return const_int(i8_, v); }
  Value boolean(bool v) { return const_int(i1_, v ? 1u : 0u); }
  Value f32c(float v);
  Value const_float(Type t, uint64_t bits);
  Value undef(Type t);
  Value null(Type t);
  Value aggregate(Type t, const std::vector<Value>& elements);
  const ValueInfo& value(Value v) const { return values_[v]; }
  Type type_of(Value v) const { return values_[v].type; }
  size_t value_count() const { return values_.size(); }
  bool is_constant(Value v) const;
  const std::vector<Value>& aggregate_elements(Value v) const { return aggregates_[values_[v].bits]; }
  // The constant's value, when `v` is an integer constant.
  bool constant_int(Value v, uint64_t& out) const;

  // Declarations and globals.
  Value declare(std::string_view name, Type fn, Attr attr);
  // A global variable of `t` in address space `space` (3: groupshared), undefined at the start.
  Value global(std::string_view name, Type t, uint32_t space, uint32_t align);

  // Metadata.
  Md md_string(std::string_view s);
  Md md_value(Value v);
  Md md_node(const std::vector<Md>& ops);  // kNone: a null operand
  Md md_u32(uint32_t v) { return md_value(u32(v)); }
  void named_metadata(std::string_view name, const std::vector<Md>& nodes);

  // The entry function's blocks; block 0 is the entry.
  Block block();
  void at(Block b) { cur_ = b; }
  Block current() const { return cur_; }
  bool terminated(Block b) const;
  size_t block_count() const { return blocks_.size(); }
  const BlockInfo& block_info(Block b) const { return blocks_[b]; }
  const Inst& inst(uint32_t i) const { return insts_[i]; }
  size_t inst_count() const { return insts_.size(); }

  // Instructions, added to the current block.
  Value bin(Bin op, Value a, Value b, uint32_t flags = 0);
  Value add(Value a, Value b) { return bin(Bin::Add, a, b); }
  Value sub(Value a, Value b) { return bin(Bin::Sub, a, b); }
  Value mul(Value a, Value b) { return bin(Bin::Mul, a, b); }
  Value and_(Value a, Value b) { return bin(Bin::And, a, b); }
  Value or_(Value a, Value b) { return bin(Bin::Or, a, b); }
  Value xor_(Value a, Value b) { return bin(Bin::Xor, a, b); }
  Value shl(Value a, Value b) { return bin(Bin::Shl, a, b); }
  Value lshr(Value a, Value b) { return bin(Bin::LShr, a, b); }
  Value ashr(Value a, Value b) { return bin(Bin::AShr, a, b); }
  Value cast(Cast op, Value v, Type to);
  Value bitcast(Value v, Type to) { return cast(Cast::Bitcast, v, to); }
  Value cmp(Pred p, Value a, Value b);
  Value select(Value c, Value a, Value b);
  Value call(Value fn, const std::vector<Value>& args);  // kNone for a void function
  Value load(Type t, Value ptr, uint32_t align);
  void store(Value ptr, Value v, uint32_t align);
  Value gep(Type source, Value base, const std::vector<Value>& indices);  // inbounds
  Value alloca_(Type t, uint32_t align);  // made in the entry block, before its terminator
  Value extract(Value aggregate, uint32_t index);
  Value atomic_rmw(Rmw op, Value ptr, Value v);  // sequentially consistent
  Value cmpxchg(Value ptr, Value expected, Value desired);  // the old value
  void br(Block to);
  void cond_br(Value c, Block then_block, Block else_block);
  void switch_(Value v, Block default_block, const std::vector<std::pair<uint32_t, Block>>& cases);
  void ret();
  void unreachable();

  // Variables: written and read in any block, made SSA by finish().
  uint32_t variable(Type t);
  Value read(uint32_t var);
  void write(uint32_t var, Value v);
  size_t variable_count() const { return var_types_.size(); }

  // DXIL intrinsics (dx.op calls); each declaration is made on first use.
  Value dx_call(uint32_t opcode, std::string_view klass, Type overload, Type result, const std::vector<Type>& params,
                Attr attr, const std::vector<Value>& args);
  Value thread_id(uint32_t axis);           // SV_DispatchThreadID
  Value group_id(uint32_t axis);            // SV_GroupID
  Value thread_id_in_group(uint32_t axis);  // SV_GroupThreadID
  Value flat_thread_id_in_group();          // SV_GroupIndex
  // Handles. A bound resource is listed in dx.resources; heap ones index the descriptor heap.
  Value bind_raw_buffer(ResourceClass c, uint32_t space, uint32_t reg);  // (RW)ByteAddressBuffer
  Value bind_cbuffer(uint32_t space, uint32_t reg, uint32_t bytes);
  Value heap_raw_buffer(Value index, bool uav, bool non_uniform);
  // A raw buffer load of `mask` (1, 3, 7, 15) elements of `t` at byte `offset`: the ResRet struct.
  Value raw_load(Type t, Value handle, Value offset, uint32_t mask, uint32_t align);
  void raw_store(Value handle, Value offset, const std::vector<Value>& v, uint32_t align);
  Value cbuffer_row(Value handle, Value row);  // 16 bytes: CBufRet.i32
  Type handle_type() const { return handle_; }

  // The entry point.
  void set_compute(uint32_t x, uint32_t y, uint32_t z, uint32_t wave_size = 0);

  // Makes variables SSA and checks every block ends. False with `error` if not.
  bool finish(std::string& error);
  bool finished() const { return finished_; }
  // After finish(): what each value became (variables' reads, trivial phis).
  Value resolve(Value v) const;
  const std::vector<Block>& order() const { return order_; }  // reachable blocks, reverse postorder

  // For the writer.
  struct FunctionInfo {
    std::string name;
    Type type = 0;  // the function type
    Attr attr = Attr::None;
    Value value = 0;
    bool body = false;
  };
  struct GlobalInfo {
    std::string name;
    Type type = 0;  // the variable's type (not the pointer)
    uint32_t space = 0, align = 0;
    Value value = 0;
  };
  struct MdInfo {
    enum Kind : uint8_t { String, Value, Node } kind = String;
    std::string text;
    uint32_t value = 0;
    std::vector<Md> ops;
  };
  const std::vector<FunctionInfo>& functions() const { return functions_; }
  const std::vector<GlobalInfo>& globals() const { return globals_; }
  const std::vector<Value>& constants() const { return constants_; }
  const std::vector<MdInfo>& metadata() const { return md_; }
  const std::vector<std::pair<std::string, std::vector<Md>>>& named() const { return named_; }
  uint64_t shader_flags() const { return flags_; }
  // After finish(): other flags (the ones the validator computed, when ours differ).
  void set_flags(uint64_t f);

 private:
  Type intern(TypeInfo t);
  Value constant(ValueKind k, Type t, uint64_t bits);
  Value add_value(ValueKind k, Type t, uint64_t bits);
  uint32_t add_inst(Inst i);
  Value emit(Op op, Type result, std::vector<uint32_t> ops, uint8_t code = 0, uint32_t aux = 0, Type type = 0);
  void terminate(Op op, std::vector<uint32_t> ops, const std::vector<Block>& succ);
  [[noreturn]] void fail(const char* what) const;
  void need(bool ok, const char* what) const {
    if (!ok) fail(what);
  }
  bool is_int(Type t) const { return types_[t].kind == TypeKind::Int; }
  bool is_float(Type t) const;
  void build_ssa();
  void entry_metadata();

  std::vector<TypeInfo> types_;
  std::unordered_map<std::string, Type> type_keys_;
  std::vector<ValueInfo> values_;
  std::unordered_map<std::string, Value> const_keys_;
  std::vector<Value> constants_;
  std::vector<std::vector<Value>> aggregates_;
  std::vector<FunctionInfo> functions_;
  std::unordered_map<std::string, uint32_t> function_names_;
  std::vector<GlobalInfo> globals_;
  std::vector<MdInfo> md_;
  std::unordered_map<std::string, Md> md_strings_;
  std::vector<std::pair<std::string, std::vector<Md>>> named_;
  std::vector<Inst> insts_;
  std::vector<BlockInfo> blocks_;
  std::vector<Type> var_types_;
  std::vector<Value> forward_;  // per value: what it became (kNone: itself)
  std::vector<Block> order_;
  Block cur_ = 0;
  bool finished_ = false;

  Type void_, i1_, i8_, i32_, i64_, f16_, f32_, f64_, handle_, res_bind_, res_props_;
  Value main_ = 0;
  // Resources listed in dx.resources.
  std::vector<Md> srvs_, uavs_, cbuffers_;
  Stage stage_ = Stage::Compute;
  uint32_t threads_[3] = {1, 1, 1};
  uint32_t wave_size_ = 0;
  uint64_t flags_ = 0;
  Md flags_md_ = kNone;
};

// The module as LLVM 3.7 bitcode (after finish()).
std::vector<uint8_t> write_bitcode(const Module& m);

// Shader flags (the entry's flags word) the validator wants: reads its "Flags declared=X,
// actual=Y" note. False if the messages have none.
bool validator_flags(std::string_view messages, uint64_t& actual);

}  // namespace am::shader::dxil
