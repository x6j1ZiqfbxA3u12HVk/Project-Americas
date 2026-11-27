// The HLSL subset compiler (hlsl.h). One pass: statements are parsed and their code made at once;
// a function is inlined at each call by parsing its body again with the call's arguments bound
// (the translator's helpers are small, and DXC inlines them all too). Locals are the module's
// variables (made SSA by Module::finish), one per scalar component; an array indexed by a value
// lives in memory (alloca), as DXC keeps it.

#include "shader/dxil/hlsl.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace am::shader::dxil {
namespace {

struct Failure {
  std::string what;
};

// ---- symbols and tokens ----

class Syms {
 public:
  uint32_t id(std::string_view s) {
    const auto it = map_.find(std::string(s));
    if (it != map_.end()) return it->second;
    names_.emplace_back(s);
    const uint32_t i = static_cast<uint32_t>(names_.size() - 1);
    map_.emplace(names_.back(), i);
    return i;
  }
  const std::string& name(uint32_t i) const { return names_[i]; }

 private:
  std::unordered_map<std::string, uint32_t> map_;
  std::vector<std::string> names_;
};

enum class Tk : uint8_t { End, Ident, Int, Float, Str, Punct, Directive };

struct Token {
  Tk kind = Tk::End;
  uint8_t suffix = 0;  // Int: 1 unsigned, 2 64-bit; Float: 1 half, 2 double, 4 no suffix
  uint32_t sym = 0;    // Ident, Punct, Str, Directive: the text
  uint32_t line = 0;
  uint64_t bits = 0;   // Int: the value; Float: the double's bits
};

double float_of(const Token& t) {
  double d = 0;
  std::memcpy(&d, &t.bits, 8);
  return d;
}

const char* const kPuncts[] = {"<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=",
                               "|=",  "^=",  "++", "--", "::", "(",  ")",  "[",  "]",  "{",  "}",  ",",  ";",  ":",  ".",  "?",
                               "+",   "-",   "*",  "/",  "%",  "&",  "|",  "^",  "~",  "!",  "=",  "<",  ">",  "#"};

// The source into tokens; each preprocessor line is one Directive token (its text, joined over
// backslash-newlines) followed by its tokens up to an End.
void lex(std::string_view src, Syms& syms, std::vector<Token>& out) {
  size_t i = 0;
  uint32_t line = 1;
  bool line_start = true;
  const auto is_id = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
  bool in_directive = false;
  while (i < src.size()) {
    const char c = src[i];
    if (c == '\n') {
      ++line;
      ++i;
      line_start = true;
      if (in_directive) {
        out.push_back({Tk::End, 0, 0, line, 0});
        in_directive = false;
      }
      continue;
    }
    if (c == '\\' && i + 1 < src.size() && (src[i + 1] == '\n' || (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n'))) {
      i += src[i + 1] == '\n' ? 2 : 3;
      ++line;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i] == '\n') ++line;
        ++i;
      }
      i += 2;
      continue;
    }
    if (c == '#' && line_start) {
      ++i;
      while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
      const size_t b = i;
      while (i < src.size() && is_id(src[i])) ++i;
      out.push_back({Tk::Directive, 0, syms.id(src.substr(b, i - b)), line, 0});
      if (src.substr(b, i - b) == "define") {  // function-like: '(' right after the name
        size_t j = i;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
        while (j < src.size() && is_id(src[j])) ++j;
        if (j < src.size() && src[j] == '(') out.back().suffix = 1;
      }
      in_directive = true;
      line_start = false;
      continue;
    }
    line_start = false;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      const size_t b = i;
      while (i < src.size() && is_id(src[i])) ++i;
      out.push_back({Tk::Ident, 0, syms.id(src.substr(b, i - b)), line, 0});
      continue;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < src.size() && src[i + 1] >= '0' && src[i + 1] <= '9')) {
      const size_t b = i;
      bool fl = false;
      if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i += 2;
        while (i < src.size() && std::isxdigit(static_cast<unsigned char>(src[i]))) ++i;
      } else {
        while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) {
          if (src[i] == '.') fl = true;
          ++i;
        }
        if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
          fl = true;
          ++i;
          if (i < src.size() && (src[i] == '+' || src[i] == '-')) ++i;
          while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
        }
      }
      const std::string text(src.substr(b, i - b));
      Token t{fl ? Tk::Float : Tk::Int, 0, 0, line, 0};
      // Suffixes: u, l, ll, ul, ull (integers); f, h, l, lf (floats).
      std::string suf;
      while (i < src.size() && (src[i] == 'u' || src[i] == 'U' || src[i] == 'l' || src[i] == 'L' || src[i] == 'f' || src[i] == 'F' ||
                                src[i] == 'h' || src[i] == 'H')) {
        suf += static_cast<char>(std::tolower(static_cast<unsigned char>(src[i++])));
      }
      if (!fl && (suf == "f" || suf == "h")) fl = true, t.kind = Tk::Float;
      if (fl) {
        const double d = std::strtod(text.c_str(), nullptr);
        std::memcpy(&t.bits, &d, 8);
        t.suffix = suf.empty() ? 4 : suf == "h" ? 1 : (suf == "l" || suf == "lf") ? 2 : 0;
      } else {
        t.bits = std::strtoull(text.c_str(), nullptr, 0);
        if (suf.find('u') != std::string::npos) t.suffix |= 1;
        if (suf.find('l') != std::string::npos || t.bits > 0xFFFFFFFFull) t.suffix |= 2;
      }
      out.push_back(t);
      continue;
    }
    if (c == '"') {
      const size_t b = ++i;
      while (i < src.size() && src[i] != '"') ++i;
      out.push_back({Tk::Str, 0, syms.id(src.substr(b, i - b)), line, 0});
      ++i;
      continue;
    }
    bool found = false;
    for (const char* p : kPuncts) {
      const size_t n = std::strlen(p);
      if (src.substr(i, n) == p) {
        out.push_back({Tk::Punct, 0, syms.id(p), line, 0});
        i += n;
        found = true;
        break;
      }
    }
    if (!found) throw Failure{"line " + std::to_string(line) + ": unexpected character '" + std::string(1, c) + "'"};
  }
  if (in_directive) out.push_back({Tk::End, 0, 0, line, 0});
  out.push_back({Tk::End, 0, 0, line, 0});
}

// ---- the preprocessor ----

struct Macro {
  bool function = false;
  std::vector<uint32_t> params;
  std::vector<Token> body;
};

class Preprocessor {
 public:
  Preprocessor(Syms& syms, std::vector<Token>& raw) : syms_(syms), raw_(raw) {
    s_define_ = syms.id("define");
    s_undef_ = syms.id("undef");
    s_ifdef_ = syms.id("ifdef");
    s_ifndef_ = syms.id("ifndef");
    s_if_ = syms.id("if");
    s_else_ = syms.id("else");
    s_endif_ = syms.id("endif");
    s_lparen_ = syms.id("(");
    s_rparen_ = syms.id(")");
    s_comma_ = syms.id(",");
  }

  std::vector<Token> run() {
    std::vector<Token> out;
    out.reserve(raw_.size());
    std::vector<bool> active{true};  // the conditional stack
    size_t i = 0;
    std::vector<Token> line;
    while (i < raw_.size() && raw_[i].kind != Tk::End) {
      const Token& t = raw_[i];
      if (t.kind == Tk::Directive) {
        // The directive's tokens, to its End.
        line.clear();
        size_t j = i + 1;
        while (j < raw_.size() && raw_[j].kind != Tk::End) line.push_back(raw_[j++]);
        const uint32_t d = t.sym;
        const bool on = active.back();
        if (d == s_ifdef_ || d == s_ifndef_) {
          const bool defined = !line.empty() && macros_.count(line[0].sym);
          active.push_back(on && (d == s_ifdef_ ? defined : !defined));
        } else if (d == s_if_) {
          active.push_back(on && !line.empty() && line[0].kind == Tk::Int && line[0].bits != 0);
        } else if (d == s_else_) {
          const bool parent = active.size() > 1 ? active[active.size() - 2] : true;
          active.back() = parent && !active.back();
        } else if (d == s_endif_) {
          if (active.size() > 1) active.pop_back();
        } else if (on && d == s_define_ && !line.empty()) {
          Macro mac;
          size_t k = 1;
          if (t.suffix == 1 && line.size() > 1 && line[1].kind == Tk::Punct && line[1].sym == s_lparen_) {
            mac.function = true;
            k = 2;
            while (k < line.size() && !(line[k].kind == Tk::Punct && line[k].sym == s_rparen_)) {
              if (line[k].kind == Tk::Ident) mac.params.push_back(line[k].sym);
              ++k;
            }
            ++k;
          }
          mac.body.assign(line.begin() + static_cast<std::ptrdiff_t>(std::min(k, line.size())), line.end());
          macros_[line[0].sym] = std::move(mac);
        } else if (on && d == s_undef_ && !line.empty()) {
          macros_.erase(line[0].sym);
        }
        i = j + 1;
        continue;
      }
      if (!active.back()) {
        ++i;
        continue;
      }
      // Ordinary tokens, macros expanded, up to the next directive.
      size_t j = i;
      while (j < raw_.size() && raw_[j].kind != Tk::Directive && raw_[j].kind != Tk::End) ++j;
      std::vector<uint32_t> hide;
      expand(raw_, i, j, out, hide);
      i = j;
    }
    out.push_back({Tk::End, 0, 0, 0, 0});
    return out;
  }

 private:
  void expand(const std::vector<Token>& src, size_t b, size_t e, std::vector<Token>& out, std::vector<uint32_t>& hide) {
    for (size_t i = b; i < e; ++i) {
      const Token& t = src[i];
      if (t.kind != Tk::Ident) {
        out.push_back(t);
        continue;
      }
      const auto it = macros_.find(t.sym);
      bool hidden = false;
      for (uint32_t h : hide) hidden |= h == t.sym;
      if (it == macros_.end() || hidden) {
        out.push_back(t);
        continue;
      }
      const Macro& mac = it->second;
      std::vector<Token> result;
      if (!mac.function) {
        result = mac.body;
      } else {
        if (i + 1 >= e || src[i + 1].kind != Tk::Punct || src[i + 1].sym != s_lparen_) {
          out.push_back(t);
          continue;
        }
        // The arguments: split at top-level commas.
        std::vector<std::vector<Token>> args(1);
        int depth = 0;
        size_t k = i + 2;
        for (; k < e; ++k) {
          const Token& a = src[k];
          if (a.kind == Tk::Punct) {
            const std::string& p = syms_.name(a.sym);
            if (p == "(" || p == "[" || p == "{") ++depth;
            else if (p == ")" || p == "]" || p == "}") {
              if (depth == 0) break;
              --depth;
            } else if (p == "," && depth == 0) {
              args.emplace_back();
              continue;
            }
          }
          args.back().push_back(a);
        }
        if (k >= e) throw Failure{"line " + std::to_string(t.line) + ": unterminated macro call " + syms_.name(t.sym)};
        if (mac.params.empty() && args.size() == 1 && args[0].empty()) args.clear();
        if (args.size() != mac.params.size()) throw Failure{"line " + std::to_string(t.line) + ": macro " + syms_.name(t.sym) + " arguments"};
        for (const Token& bt : mac.body) {
          bool param = false;
          if (bt.kind == Tk::Ident) {
            for (size_t p = 0; p < mac.params.size(); ++p) {
              if (mac.params[p] == bt.sym) {
                result.insert(result.end(), args[p].begin(), args[p].end());
                param = true;
                break;
              }
            }
          }
          if (!param) result.push_back(bt);
        }
        i = k;
      }
      for (Token& r : result) r.line = t.line;
      hide.push_back(t.sym);
      expand(result, 0, result.size(), out, hide);
      hide.pop_back();
    }
  }

  Syms& syms_;
  std::vector<Token>& raw_;
  std::unordered_map<uint32_t, Macro> macros_;
  uint32_t s_define_, s_undef_, s_ifdef_, s_ifndef_, s_if_, s_else_, s_endif_, s_lparen_, s_rparen_, s_comma_;
};

// ---- types ----

enum class B : uint8_t { Void, Bool, Int, Uint, Int64, Uint64, Half, Float, Double, Struct, Res, HeapRef };

// Resource kinds the subset takes.
enum class RK : uint8_t {
  Raw, RWRaw, Tex1D, Tex1DArr, Tex2D, Tex2DArr, Tex2DMS, Tex2DMSArr, Tex3D, TexCube, TexCubeArr,
  RWTex1D, RWTex1DArr, RWTex2D, RWTex2DArr, RWTex3D, Sampler, SamplerCmp,
};

struct Ty {
  B b = B::Void;
  uint8_t n = 1;     // vector components
  RK rk = RK::Raw;   // Res
  B elem = B::Float; // Res: a typed view's element
  uint8_t en = 4;
  uint32_t st = 0;   // Struct: its index
  uint32_t arr = 0;  // an array's length (0: none)
  bool operator==(const Ty& o) const {
    return b == o.b && n == o.n && rk == o.rk && elem == o.elem && en == o.en && st == o.st && arr == o.arr;
  }
};

bool is_int(B b) { return b == B::Bool || b == B::Int || b == B::Uint || b == B::Int64 || b == B::Uint64; }
bool is_float(B b) { return b == B::Half || b == B::Float || b == B::Double; }
bool is_signed(B b) { return b == B::Int || b == B::Int64; }
bool is_numeric(B b) { return is_int(b) || is_float(b); }
int rank(B b) {
  switch (b) {
    case B::Bool: return 0;
    case B::Int: return 1;
    case B::Uint: return 2;
    case B::Int64: return 3;
    case B::Uint64: return 4;
    case B::Half: return 5;
    case B::Float: return 6;
    case B::Double: return 7;
    default: return -1;
  }
}

Ty scalar(B b, uint8_t n = 1) {
  Ty t;
  t.b = b;
  t.n = n;
  return t;
}

struct StructDef {
  uint32_t name = 0;
  std::vector<std::pair<uint32_t, Ty>> members;
};

// An rvalue: its scalar components, flattened (a vector's, a struct's members in order, an
// array's elements). `lit` marks a literal (HLSL's literal int and float take the other operand's
// type).
struct RV {
  Ty t;
  std::vector<Value> c;
  bool lit = false;
  bool non_uniform = false;  // NonUniformResourceIndex
  bool sampler_heap = false; // HeapRef: SamplerDescriptorHeap
};

// An lvalue: variables (one per component), memory (a pointer per component), or a texel of a
// writable texture.
struct LV {
  // Pick: one component of a vector in variables (vars), chosen by a value (pick).
  enum Kind : uint8_t { Vars, Mem, Texel, Pick } kind = Vars;
  Ty t;
  std::vector<uint32_t> vars;
  std::vector<Value> ptrs;     // Mem: an element's pointer per component
  Value arr = kNone;           // Mem: a whole array (alloca or global), indexed into ptrs
  Type arr_type = 0;
  Value handle = kNone;        // Texel
  std::vector<Value> coords;
  Value pick = kNone;
};

struct Ex {
  bool is_lv = false;
  LV lv;
  RV rv;
};

struct Param {
  uint32_t name = 0;
  Ty t;
  uint8_t dir = 0;  // 0 in, 1 out, 2 inout
  size_t def_begin = 0, def_end = 0;  // a default value's tokens
};

struct Fn {
  uint32_t name = 0;
  Ty ret;
  std::vector<Param> params;
  size_t body = 0;  // the token after '{'
  size_t attr_begin = 0, attr_end = 0;
  std::vector<uint32_t> semantics;  // per parameter (entry points)
  bool dynamic_arrays_known = false;
  std::unordered_set<uint32_t> dynamic_arrays;  // local arrays indexed by a value
};

struct Local {
  Ty t;
  std::vector<uint32_t> vars;  // a variable per component
  Value mem = kNone;           // an array in memory: the alloca (of [n x elem])
  Value handle = kNone;        // a resource: its handle
};

// Globals: bound resources, cbuffer members, groupshared and static const arrays.
struct Global {
  enum Kind : uint8_t { Resource, CbMember, Shared, Constant } kind = Resource;
  Ty t;
  ResourceClass cls = ResourceClass::Srv;
  uint32_t space = 0, reg = 0;
  Value handle = kNone;  // made at first use, in the entry block
  uint32_t cb = 0;       // CbMember: its cbuffer
  uint32_t offset = 0;   // CbMember: byte offset
  // Shared, Constant: the global, made at first use (the validator refuses an unused internal
  // global, and the translator's prelude declares some that a program may not touch).
  Value ptr = kNone;
  uint32_t name = 0;
  std::vector<Value> init;  // Constant: the elements
};

struct CBuffer {
  uint32_t space = 0, reg = 0, bytes = 0;
  Value handle = kNone;
};

class Compiler {
 public:
  Compiler(Module& m, Syms& syms, std::vector<Token> toks) : m_(m), syms_(syms), t_(std::move(toks)) { names(); }

  void run(std::string_view entry) {
    top_level();
    const auto it = fns_.find(syms_.id(entry));
    if (it == fns_.end()) throw Failure{"no entry function " + std::string(entry)};
    compile_entry(*it->second);
  }

 private:
  // ---- names ----
  uint32_t s_struct, s_cbuffer, s_register, s_static, s_const, s_groupshared, s_in, s_out, s_inout, s_if, s_else, s_for,
      s_while, s_do, s_switch, s_case, s_default, s_break, s_continue, s_return, s_goto, s_true, s_false, s_void, s_uniform,
      s_precise, s_nointerp, s_heap, s_sheap, s_nonuniform, s_numthreads, s_wavesize;
  uint32_t p_lparen, p_rparen, p_lbrack, p_rbrack, p_lbrace, p_rbrace, p_comma, p_semi, p_colon, p_dot, p_quest, p_assign, p_lt,
      p_gt;
  void names() {
    const auto s = [&](const char* x) { return syms_.id(x); };
    s_struct = s("struct"), s_cbuffer = s("cbuffer"), s_register = s("register"), s_static = s("static"), s_const = s("const");
    s_groupshared = s("groupshared"), s_in = s("in"), s_out = s("out"), s_inout = s("inout"), s_if = s("if"), s_else = s("else");
    s_for = s("for"), s_while = s("while"), s_do = s("do"), s_switch = s("switch"), s_case = s("case"), s_default = s("default");
    s_break = s("break"), s_continue = s("continue"), s_return = s("return"), s_goto = s("goto"), s_true = s("true");
    s_false = s("false"), s_void = s("void"), s_uniform = s("uniform"), s_precise = s("precise"), s_nointerp = s("nointerpolation");
    s_heap = s("ResourceDescriptorHeap"), s_sheap = s("SamplerDescriptorHeap"), s_nonuniform = s("NonUniformResourceIndex");
    s_numthreads = s("numthreads"), s_wavesize = s("WaveSize");
    p_lparen = s("("), p_rparen = s(")"), p_lbrack = s("["), p_rbrack = s("]"), p_lbrace = s("{"), p_rbrace = s("}");
    p_comma = s(","), p_semi = s(";"), p_colon = s(":"), p_dot = s("."), p_quest = s("?"), p_assign = s("="), p_lt = s("<");
    p_gt = s(">");
  }

  // ---- token access ----
  const Token& peek(size_t k = 0) const { return t_[std::min(pos_ + k, t_.size() - 1)]; }
  const Token& next() { return t_[pos_ < t_.size() - 1 ? pos_++ : pos_]; }
  bool is_p(const Token& t, uint32_t p) const { return t.kind == Tk::Punct && t.sym == p; }
  bool is_p(uint32_t p, size_t k = 0) const { return is_p(peek(k), p); }
  bool is_id(uint32_t s, size_t k = 0) const { return peek(k).kind == Tk::Ident && peek(k).sym == s; }
  bool accept(uint32_t p) {
    if (!is_p(p)) return false;
    ++pos_;
    return true;
  }
  bool accept_id(uint32_t s) {
    if (!is_id(s)) return false;
    ++pos_;
    return true;
  }
  [[noreturn]] void fail(const std::string& what) const {
    throw Failure{"line " + std::to_string(peek().line) + ": " + what + " (at '" + text(peek()) + "')"};
  }
  std::string text(const Token& t) const {
    switch (t.kind) {
      case Tk::Ident:
      case Tk::Punct:
      case Tk::Str: return syms_.name(t.sym);
      case Tk::Int: return std::to_string(t.bits);
      case Tk::Float: return std::to_string(float_of(t));
      default: return "end";
    }
  }
  void expect(uint32_t p) {
    if (!accept(p)) fail("expected '" + syms_.name(p) + "'");
  }
  uint32_t ident() {
    if (peek().kind != Tk::Ident) fail("expected a name");
    return next().sym;
  }
  // Skips a balanced group starting at the current '(' / '[' / '{'.
  void skip_group() {
    int depth = 0;
    do {
      const Token& t = next();
      if (t.kind == Tk::End) fail("unbalanced brackets");
      if (t.kind == Tk::Punct) {
        if (t.sym == p_lparen || t.sym == p_lbrack || t.sym == p_lbrace) ++depth;
        else if (t.sym == p_rparen || t.sym == p_rbrack || t.sym == p_rbrace) --depth;
      }
    } while (depth > 0);
  }
  // [attribute(...)] groups before a statement or function.
  void skip_attributes() {
    while (is_p(p_lbrack) && peek(1).kind == Tk::Ident) skip_group();
  }

  // ---- types ----
  bool type_name(size_t k = 0) const {
    const Token& t = peek(k);
    if (t.kind != Tk::Ident) return false;
    Ty ty;
    return parse_type_name(syms_.name(t.sym), ty) || structs_by_name_.count(t.sym) || resource_kind(syms_.name(t.sym), ty.rk);
  }
  static bool parse_type_name(const std::string& n, Ty& t) {
    static const std::pair<const char*, B> bases[] = {{"uint64_t", B::Uint64}, {"int64_t", B::Int64}, {"bool", B::Bool},
                                                      {"uint", B::Uint},       {"dword", B::Uint},     {"int", B::Int},
                                                      {"half", B::Half},       {"float", B::Float},    {"double", B::Double},
                                                      {"min16float", B::Half}, {"min16int", B::Int},   {"min16uint", B::Uint}};
    for (const auto& [name, b] : bases) {
      const size_t len = std::strlen(name);
      if (n.compare(0, len, name) != 0) continue;
      if (n.size() == len) {
        t = scalar(b);
        return true;
      }
      if (n.size() == len + 1 && n[len] >= '1' && n[len] <= '4') {
        t = scalar(b, static_cast<uint8_t>(n[len] - '0'));
        return true;
      }
    }
    return false;
  }
  static bool resource_kind(const std::string& n, RK& k) {
    static const std::pair<const char*, RK> kinds[] = {
        {"ByteAddressBuffer", RK::Raw},       {"RWByteAddressBuffer", RK::RWRaw}, {"Texture1D", RK::Tex1D},
        {"Texture1DArray", RK::Tex1DArr},     {"Texture2D", RK::Tex2D},           {"Texture2DArray", RK::Tex2DArr},
        {"Texture2DMS", RK::Tex2DMS},         {"Texture2DMSArray", RK::Tex2DMSArr}, {"Texture3D", RK::Tex3D},
        {"TextureCube", RK::TexCube},         {"TextureCubeArray", RK::TexCubeArr}, {"RWTexture1D", RK::RWTex1D},
        {"RWTexture1DArray", RK::RWTex1DArr}, {"RWTexture2D", RK::RWTex2D},       {"RWTexture2DArray", RK::RWTex2DArr},
        {"RWTexture3D", RK::RWTex3D},         {"SamplerState", RK::Sampler},      {"SamplerComparisonState", RK::SamplerCmp}};
    for (const auto& [name, kind] : kinds) {
      if (n == name) {
        k = kind;
        return true;
      }
    }
    return false;
  }
  Ty parse_type() {
    const uint32_t s = ident();
    const std::string& n = syms_.name(s);
    Ty t;
    if (parse_type_name(n, t)) return t;
    const auto st = structs_by_name_.find(s);
    if (st != structs_by_name_.end()) {
      t.b = B::Struct;
      t.st = st->second;
      return t;
    }
    if (resource_kind(n, t.rk)) {
      t.b = B::Res;
      if (accept(p_lt)) {
        const Ty e = parse_type();
        t.elem = e.b;
        t.en = e.n;
        expect(p_gt);
      }
      return t;
    }
    fail("unknown type " + n);
  }
  size_t comps(const Ty& t) const {
    size_t one = 1;
    if (t.b == B::Struct) {
      one = 0;
      for (const auto& [name, mt] : structs_[t.st].members) one += comps(mt);
    } else if (t.b != B::Res && t.b != B::HeapRef) {
      one = t.n;
    }
    return t.arr ? t.arr * one : one;
  }
  Type ir(B b) const {
    switch (b) {
      case B::Bool: return m_.i1();
      case B::Int:
      case B::Uint: return m_.i32();
      case B::Int64:
      case B::Uint64: return m_.i64();
      case B::Half:
      case B::Float: return m_.f32();
      case B::Double: return m_.f64();
      default: break;
    }
    fail("a value of no scalar type");
  }
  // Each component's base type, in flattened order.
  void bases(const Ty& t, std::vector<B>& out) const {
    const size_t times = t.arr ? t.arr : 1;
    for (size_t r = 0; r < times; ++r) {
      if (t.b == B::Struct) {
        for (const auto& [name, mt] : structs_[t.st].members) bases(mt, out);
      } else {
        for (int k = 0; k < t.n; ++k) out.push_back(t.b);
      }
    }
  }
  Ty element(Ty t) const {
    t.arr = 0;
    return t;
  }

  // ---- values ----
  Value zero(B b) {
    if (is_float(b)) return b == B::Double ? m_.const_float(m_.f64(), 0) : m_.f32c(0.0f);
    return m_.const_int(ir(b), 0);
  }
  Value neg_zero(B b) { return b == B::Double ? m_.const_float(m_.f64(), 0x8000000000000000ull) : m_.const_float(m_.f32(), 0x80000000u); }
  Value conv(Value v, B from, B to) {
    if (from == to) return v;
    if (to == B::Bool) {
      if (is_float(from)) return m_.cmp(Pred::FUne, v, zero(from), kFastMath);
      return m_.cmp(Pred::INe, v, zero(from));
    }
    if (from == B::Bool) {
      if (is_float(to)) return m_.select(v, to == B::Double ? m_.const_float(m_.f64(), 0x3FF0000000000000ull) : m_.f32c(1.0f), zero(to));
      return m_.cast(Cast::ZExt, v, ir(to));
    }
    const Type ti = ir(to), fi = ir(from);
    if (is_int(from) && is_int(to)) {
      if (ti == fi) return v;
      if (m_.type(ti).n < m_.type(fi).n) return m_.cast(Cast::Trunc, v, ti);
      return m_.cast(is_signed(from) ? Cast::SExt : Cast::ZExt, v, ti);
    }
    if (is_int(from)) return m_.cast(is_signed(from) ? Cast::SiToFp : Cast::UiToFp, v, ti);
    if (is_int(to)) return m_.cast(is_signed(to) ? Cast::FpToSi : Cast::FpToUi, v, ti);
    if (ti == fi) return v;
    return m_.cast(m_.type(ti).kind == TypeKind::Double ? Cast::FpExt : Cast::FpTrunc, v, ti);
  }
  // `r` as type `to`: scalars splat, vectors truncate, numbers convert per component.
  RV convert(const RV& r, const Ty& to) {
    if (r.t == to) return r;
    RV out;
    out.t = to;
    if (to.b == B::Struct || to.arr) {
      std::vector<B> tb;
      bases(to, tb);
      if (r.t.b != B::Struct && !r.t.arr && r.c.size() == 1) {  // (T)0 and the like: every member
        for (B b : tb) out.c.push_back(conv(r.c[0], r.t.b, b));
        return out;
      }
      if (r.t == to || r.c.size() == tb.size()) {
        std::vector<B> fb;
        bases(r.t, fb);
        for (size_t k = 0; k < tb.size(); ++k) out.c.push_back(conv(r.c[k], fb[k], tb[k]));
        return out;
      }
      fail("a conversion between aggregates");
    }
    if (r.t.b == B::Struct || r.t.arr || r.t.b == B::Res || to.b == B::Res) fail("a conversion of an aggregate or resource");
    for (int k = 0; k < to.n; ++k) {
      const Value x = r.c.size() == 1 ? r.c[0] : k < static_cast<int>(r.c.size()) ? r.c[static_cast<size_t>(k)] : kNone;
      if (x == kNone) fail("a vector made wider");
      out.c.push_back(conv(x, r.t.b, to.b));
    }
    return out;
  }
  RV with_base(const RV& r, B b) {
    Ty t = r.t;
    t.b = b;
    return convert(r, t);
  }
  Value scalar_of(const RV& r, B b) {
    if (r.c.empty()) fail("a value with no components");
    return conv(r.c[0], r.t.b, b);
  }
  RV make(B b, std::vector<Value> c) {
    RV r;
    r.t = scalar(b, static_cast<uint8_t>(c.size()));
    r.c = std::move(c);
    return r;
  }
  RV u32(uint32_t v) { return make(B::Uint, {m_.u32(v)}); }
  RV one() {
    RV r = make(B::Int, {m_.u32(1)});
    r.lit = true;
    return r;
  }

  // ---- reading and writing lvalues ----
  RV load(const LV& lv) {
    RV r;
    r.t = lv.t;
    std::vector<B> tb;
    bases(lv.t, tb);
    switch (lv.kind) {
      case LV::Vars:
        for (uint32_t v : lv.vars) r.c.push_back(m_.read(v));
        break;
      case LV::Mem:
        if (lv.arr != kNone) fail("a whole array in memory read");
        for (size_t k = 0; k < lv.ptrs.size(); ++k) r.c.push_back(m_.load(ir(tb[k]), lv.ptrs[k], 4));
        break;
      case LV::Texel: return texel_load(lv);
      case LV::Pick: r.c.push_back(pick_read(lv)); break;
    }
    return r;
  }
  Value pick_read(const LV& lv) {
    Value r = m_.read(lv.vars.back());
    for (size_t c = lv.vars.size() - 1; c-- > 0;) r = m_.select(m_.cmp(Pred::IEq, lv.pick, m_.u32(static_cast<uint32_t>(c))), m_.read(lv.vars[c]), r);
    return r;
  }
  void store(const LV& lv, const RV& value) {
    const RV v = convert(value, lv.t);
    switch (lv.kind) {
      case LV::Vars:
        for (size_t k = 0; k < lv.vars.size(); ++k) m_.write(lv.vars[k], v.c[k]);
        break;
      case LV::Mem:
        if (lv.arr != kNone) fail("a whole array in memory written");
        for (size_t k = 0; k < lv.ptrs.size(); ++k) m_.store(lv.ptrs[k], v.c[k], 4);
        break;
      case LV::Texel: texel_store(lv, v); break;
      case LV::Pick:
        for (size_t k = 0; k < lv.vars.size(); ++k)
          m_.write(lv.vars[k], m_.select(m_.cmp(Pred::IEq, lv.pick, m_.u32(static_cast<uint32_t>(k))), v.c[0], m_.read(lv.vars[k])));
        break;
    }
  }
  RV value(const Ex& e) { return e.is_lv ? load(e.lv) : e.rv; }
  Ex rv(RV r) {
    Ex e;
    e.rv = std::move(r);
    return e;
  }

  // ---- scopes ----
  // A deque: emplace_back and pop_back never move the other scopes, so a Local* stays valid while
  // its scope lives (a vector of maps copies them when it grows: MSVC's map move may throw).
  std::deque<std::unordered_map<uint32_t, Local>> scopes_;
  Local* find_local(uint32_t name) {
    for (size_t i = scopes_.size(); i-- > 0;) {
      const auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) return &it->second;
    }
    return nullptr;
  }
  Local& declare(uint32_t name, const Ty& t, bool in_memory) {
    Local l;
    l.t = t;
    std::vector<B> tb;
    bases(t, tb);
    if (t.b == B::Res || t.b == B::HeapRef) {
      // a resource: bound to its handle when initialized
    } else if (in_memory && t.arr) {
      Ty e = element(t);
      if (e.b == B::Struct) fail("an array of structs indexed by a value");
      // Vectors flat: element k, component j at k * n + j.
      l.mem = m_.alloca_(m_.array(ir(e.b), t.arr * e.n), 4);
    } else {
      for (B b : tb) l.vars.push_back(m_.variable(ir(b)));
    }
    auto& scope = scopes_.back();
    scope[name] = std::move(l);
    return scope[name];
  }
  LV local_lv(Local& l) {
    LV lv;
    lv.t = l.t;
    if (l.mem != kNone) {
      lv.kind = LV::Mem;
      lv.arr = l.mem;
      lv.arr_type = m_.array(ir(l.t.b), l.t.arr * l.t.n);
      return lv;
    }
    lv.vars = l.vars;
    return lv;
  }

  // ---- control flow state ----
  struct Loop {
    Block brk, cont;
  };
  std::vector<Loop> loops_;
  struct Ret {
    Block exit;
    std::vector<uint32_t> vars;
    Ty t;
  };
  std::vector<Ret> rets_;
  std::unordered_map<uint32_t, Block> labels_;
  Block label_block(uint32_t name) {
    const auto it = labels_.find(name);
    if (it != labels_.end()) return it->second;
    const Block b = m_.block();
    labels_[name] = b;
    return b;
  }
  // After a branch out (return, break, continue, goto): later code goes into a block nothing
  // reaches (finish() drops it).
  void dead() { m_.at(m_.block()); }
  void branch(Block to) {
    if (!m_.terminated(m_.current())) m_.br(to);
  }

  // ---- top level ----
  std::vector<StructDef> structs_;
  std::unordered_map<uint32_t, uint32_t> structs_by_name_;
  std::unordered_map<uint32_t, std::unique_ptr<Fn>> fns_;
  std::unordered_map<uint32_t, Global> globals_;
  std::vector<CBuffer> cbuffers_;

  void top_level() {
    while (peek().kind != Tk::End) {
      if (accept(p_semi)) continue;
      if (is_id(s_struct)) {
        struct_def();
        continue;
      }
      if (is_id(s_cbuffer)) {
        cbuffer_def();
        continue;
      }
      const size_t attr_begin = pos_;
      skip_attributes();
      const size_t attr_end = pos_;
      bool is_static = false, is_const = false, shared = false;
      for (;;) {
        if (accept_id(s_static)) is_static = true;
        else if (accept_id(s_const)) is_const = true;
        else if (accept_id(s_groupshared)) shared = true;
        else if (accept_id(s_uniform) || accept_id(s_precise)) continue;
        else break;
      }
      Ty t;
      if (accept_id(s_void)) t.b = B::Void;
      else t = parse_type();
      const uint32_t name = ident();
      if (is_p(p_lparen)) {
        function_def(t, name, attr_begin, attr_end);
        continue;
      }
      // A global: a bound resource, groupshared or static const array.
      if (accept(p_lbrack)) {
        if (peek().kind != Tk::Int) fail("an array size not a number");
        t.arr = static_cast<uint32_t>(next().bits);
        expect(p_rbrack);
      }
      Global g;
      g.t = t;
      if (t.b == B::Res) {
        g.kind = Global::Resource;
        register_of(g.cls, g.space, g.reg);
        if (t.rk != RK::Raw && t.rk != RK::RWRaw) fail("a bound resource other than a raw buffer");
      } else if (shared) {
        g.kind = Global::Shared;
        if (!t.arr || t.n != 1 || !is_numeric(t.b)) fail("groupshared other than an array of scalars");
        g.name = name;
      } else if (is_static && is_const) {
        g.kind = Global::Constant;
        if (!t.arr || t.n != 1 || !is_numeric(t.b)) fail("a static const other than an array of scalars");
        expect(p_assign);
        expect(p_lbrace);
        std::vector<Value> elems;
        while (!accept(p_rbrace)) {
          const RV v = convert(constant_expr(), element(t));
          elems.push_back(v.c[0]);
          accept(p_comma);
        }
        if (elems.size() != t.arr) fail("a static const array with the wrong count");
        g.name = name;
        g.init = std::move(elems);
      } else {
        fail("a global variable outside the subset");
      }
      expect(p_semi);
      globals_[name] = g;
    }
  }
  void register_of(ResourceClass& cls, uint32_t& space, uint32_t& reg) {
    expect(p_colon);
    if (!accept_id(s_register)) fail("a resource without a register");
    expect(p_lparen);
    const std::string r = syms_.name(ident());
    if (r.size() < 2) fail("a register name");
    cls = r[0] == 'u' ? ResourceClass::Uav : r[0] == 't' ? ResourceClass::Srv : r[0] == 'b' ? ResourceClass::CBuffer : ResourceClass::Sampler;
    reg = static_cast<uint32_t>(std::stoul(r.substr(1)));
    space = 0;
    if (accept(p_comma)) {
      const std::string sp = syms_.name(ident());
      space = static_cast<uint32_t>(std::stoul(sp.substr(5)));
    }
    expect(p_rparen);
  }
  // A constant initializer element (a number, maybe negated).
  RV constant_expr() {
    bool neg = accept(syms_.id("-"));
    const Token& t = next();
    if (t.kind == Tk::Int) {
      RV r = make(t.suffix & 2 ? (t.suffix & 1 ? B::Uint64 : B::Int64) : (t.suffix & 1 ? B::Uint : B::Int),
                  {m_.const_int(t.suffix & 2 ? m_.i64() : m_.i32(), neg ? 0 - t.bits : t.bits)});
      return r;
    }
    if (t.kind == Tk::Float) return make(B::Float, {m_.f32c(static_cast<float>(neg ? -float_of(t) : float_of(t)))});
    fail("a constant initializer that is not a number");
  }
  void struct_def() {
    accept_id(s_struct);
    StructDef sd;
    sd.name = ident();
    expect(p_lbrace);
    while (!accept(p_rbrace)) {
      while (accept_id(s_nointerp) || accept_id(s_precise)) {
      }
      const Ty base = parse_type();
      do {
        const uint32_t mn = ident();
        Ty mt = base;
        if (accept(p_lbrack)) {
          mt.arr = static_cast<uint32_t>(next().bits);
          expect(p_rbrack);
        }
        if (accept(p_colon)) ident();  // a semantic
        sd.members.emplace_back(mn, mt);
      } while (accept(p_comma));
      expect(p_semi);
    }
    expect(p_semi);
    structs_by_name_[sd.name] = static_cast<uint32_t>(structs_.size());
    structs_.push_back(std::move(sd));
  }
  void cbuffer_def() {
    accept_id(s_cbuffer);
    ident();
    CBuffer cb;
    ResourceClass cls;
    register_of(cls, cb.space, cb.reg);
    const uint32_t index = static_cast<uint32_t>(cbuffers_.size());
    expect(p_lbrace);
    uint32_t offset = 0;  // bytes; HLSL packing: an array starts a row, every element a row; no vector crosses a row
    while (!accept(p_rbrace)) {
      const Ty base = parse_type();
      const uint32_t name = ident();
      Ty t = base;
      if (accept(p_lbrack)) {
        t.arr = static_cast<uint32_t>(next().bits);
        expect(p_rbrack);
      }
      expect(p_semi);
      if (t.b == B::Struct || !is_numeric(t.b) || t.b == B::Double || t.b == B::Int64 || t.b == B::Uint64) fail("a cbuffer member outside the subset");
      const uint32_t size = 4u * t.n;
      if (t.arr || (offset % 16) + size > 16) offset = (offset + 15) & ~15u;
      Global g;
      g.kind = Global::CbMember;
      g.t = t;
      g.cb = index;
      g.offset = offset;
      globals_[name] = g;
      offset += t.arr ? 16u * t.arr : size;
    }
    accept(p_semi);
    cb.bytes = (offset + 15) & ~15u;
    cbuffers_.push_back(cb);
  }
  void function_def(const Ty& ret, uint32_t name, size_t attr_begin, size_t attr_end) {
    auto f = std::make_unique<Fn>();
    f->name = name;
    f->ret = ret;
    f->attr_begin = attr_begin;
    f->attr_end = attr_end;
    expect(p_lparen);
    while (!accept(p_rparen)) {
      Param p;
      for (;;) {
        if (accept_id(s_in)) p.dir = p.dir;
        else if (accept_id(s_out)) p.dir = 1;
        else if (accept_id(s_inout)) p.dir = 2;
        else if (accept_id(s_nointerp) || accept_id(s_const) || accept_id(s_uniform) || accept_id(syms_.id("vertices")) ||
                 accept_id(syms_.id("indices")) || accept_id(syms_.id("primitives")))
          continue;
        else break;
      }
      p.t = parse_type();
      p.name = ident();
      if (accept(p_lbrack)) {
        p.t.arr = static_cast<uint32_t>(next().bits);
        expect(p_rbrack);
      }
      uint32_t sem = 0;
      if (accept(p_colon)) sem = ident();
      if (accept(p_assign)) {
        p.def_begin = pos_;
        while (!is_p(p_comma) && !is_p(p_rparen)) next();
        p.def_end = pos_;
      }
      f->params.push_back(p);
      f->semantics.push_back(sem);
      accept(p_comma);
    }
    if (accept(p_colon)) ident();  // a return semantic
    if (!is_p(p_lbrace)) fail("a function declaration without a body");
    f->body = pos_ + 1;
    skip_group();
    fns_[name] = std::move(f);
  }

  // Local arrays a function indexes by a value (they live in memory).
  void find_dynamic_arrays(Fn& f) {
    if (f.dynamic_arrays_known) return;
    f.dynamic_arrays_known = true;
    int depth = 0;
    for (size_t i = f.body; i < t_.size(); ++i) {
      const Token& t = t_[i];
      if (t.kind == Tk::Punct && t.sym == p_lbrace) ++depth;
      if (t.kind == Tk::Punct && t.sym == p_rbrace && depth-- == 0) break;
      if (t.kind == Tk::Ident && i + 1 < t_.size() && is_p(t_[i + 1], p_lbrack)) {
        const bool literal = i + 3 < t_.size() && t_[i + 2].kind == Tk::Int && is_p(t_[i + 3], p_rbrack);
        if (!literal) f.dynamic_arrays.insert(t.sym);
      }
    }
  }

  // ---- the entry point ----
  Fn* fn_ = nullptr;  // the function whose body is being compiled (innermost inline)

  void compile_entry(Fn& f) {
    // Attributes: numthreads, WaveSize.
    uint32_t threads[3] = {1, 1, 1}, wave = 0;
    bool compute = false;
    const size_t keep = pos_;
    pos_ = f.attr_begin;
    while (pos_ < f.attr_end) {
      expect(p_lbrack);
      const uint32_t a = ident();
      std::vector<uint32_t> nums;
      if (accept(p_lparen)) {
        while (!accept(p_rparen)) {
          if (peek().kind == Tk::Int) nums.push_back(static_cast<uint32_t>(peek().bits));
          next();
        }
      }
      expect(p_rbrack);
      if (a == s_numthreads && nums.size() == 3) {
        compute = true;
        for (int k = 0; k < 3; ++k) threads[k] = nums[static_cast<size_t>(k)];
      } else if (a == s_wavesize && !nums.empty()) {
        wave = nums[0];
      }
    }
    pos_ = keep;
    if (!compute) fail("an entry point other than a compute shader (not yet)");
    m_.set_compute(threads[0], threads[1], threads[2], wave);
    // Block 0 holds handles and allocas made on first use; the code starts in block 1.
    const Block start = m_.block();
    m_.at(start);
    scopes_.emplace_back();
    for (size_t k = 0; k < f.params.size(); ++k) {
      const Param& p = f.params[k];
      const std::string sem = f.semantics[k] ? syms_.name(f.semantics[k]) : std::string();
      Local& l = declare(p.name, p.t, false);
      for (uint32_t c = 0; c < p.t.n; ++c) {
        Value v;
        if (sem == "SV_GroupThreadID") v = m_.thread_id_in_group(c);
        else if (sem == "SV_GroupID") v = m_.group_id(c);
        else if (sem == "SV_DispatchThreadID") v = m_.thread_id(c);
        else if (sem == "SV_GroupIndex") v = m_.flat_thread_id_in_group();
        else fail("an entry parameter with semantic " + sem);
        m_.write(l.vars[c], v);
      }
    }
    fn_ = &f;
    find_dynamic_arrays(f);
    Ret r;
    r.exit = m_.block();
    rets_.push_back(r);
    pos_ = f.body;
    block_body();
    branch(r.exit);
    rets_.pop_back();
    m_.at(r.exit);
    m_.ret();
    m_.at(0);
    m_.br(start);
    scopes_.pop_back();
  }

  // Statements to the closing '}' (consumed).
  void block_body() {
    scopes_.emplace_back();
    while (!accept(p_rbrace)) {
      if (peek().kind == Tk::End) fail("a block without its '}'");
      statement();
    }
    scopes_.pop_back();
  }

  void statement() {
    skip_attributes();
    const Token& t = peek();
    if (t.kind == Tk::Punct) {
      if (accept(p_lbrace)) {
        block_body();
        return;
      }
      if (accept(p_semi)) return;
    }
    if (t.kind == Tk::Ident) {
      const uint32_t s = t.sym;
      if (s == s_if) return if_statement();
      if (s == s_for) return for_statement();
      if (s == s_while) return while_statement();
      if (s == s_do) return do_statement();
      if (s == s_switch) return switch_statement();
      if (s == s_return) return return_statement();
      if (s == s_break || s == s_continue) {
        next();
        expect(p_semi);
        if (loops_.empty()) fail("break or continue outside a loop");
        m_.br(s == s_break ? loops_.back().brk : loops_.back().cont);
        dead();
        return;
      }
      if (s == s_goto) {
        next();
        const uint32_t l = ident();
        expect(p_semi);
        m_.br(label_block(l));
        dead();
        return;
      }
      // A label: name ':' (not '::').
      if (is_p(p_colon, 1) && !type_name()) {
        const uint32_t l = next().sym;
        next();
        const Block b = label_block(l);
        branch(b);
        m_.at(b);
        return;
      }
      if (s == s_const || s == s_static || s == s_precise || type_name()) {
        declaration();
        return;
      }
    }
    expression();
    expect(p_semi);
  }

  void declaration() {
    bool is_const = false;
    while (accept_id(s_const) || accept_id(s_static) || accept_id(s_precise)) is_const = true;
    (void)is_const;
    const Ty base = parse_type();
    do {
      const uint32_t name = ident();
      Ty t = base;
      if (accept(p_lbrack)) {
        if (peek().kind != Tk::Int) fail("a local array whose size is not a number");
        t.arr = static_cast<uint32_t>(next().bits);
        expect(p_rbrack);
      }
      const bool in_memory = t.arr && fn_ && fn_->dynamic_arrays.count(name);
      if (accept(p_assign)) {
        if (t.b == B::Res) {
          const RV init = value(assignment());
          Local& l = declare(name, t, false);
          l.handle = resource_handle(t, init);
        } else if (is_p(p_lbrace)) {
          // An initializer list: the components of its values in order (nested braces flatten),
          // each converted to the declared base type, as many as the declaration has.
          if (!is_numeric(t.b)) fail("an initializer list for a struct");
          std::vector<Value> comps_in;
          init_list(t.b, comps_in);
          if (comps_in.size() != comps(t)) fail("an initializer list of the wrong size");
          Local& l = declare(name, t, in_memory);
          if (l.mem != kNone) {
            const Type at = m_.array(ir(t.b), t.arr * t.n);
            for (size_t k = 0; k < comps_in.size(); ++k)
              m_.store(m_.gep(at, l.mem, {m_.u32(0), m_.u32(static_cast<uint32_t>(k))}), comps_in[k], 4);
          } else {
            for (size_t k = 0; k < comps_in.size(); ++k) m_.write(l.vars[k], comps_in[k]);
          }
        } else {
          const RV init = value(assignment());
          Local& l = declare(name, t, in_memory);
          store(local_lv(l), init);
        }
      } else {
        declare(name, t, in_memory);
      }
    } while (accept(p_comma));
    expect(p_semi);
  }

  void init_list(B base, std::vector<Value>& out) {
    expect(p_lbrace);
    while (!accept(p_rbrace)) {
      if (is_p(p_lbrace)) {
        init_list(base, out);
      } else {
        const RV v = with_base(value(assignment()), base);
        out.insert(out.end(), v.c.begin(), v.c.end());
      }
      accept(p_comma);
    }
  }

  void if_statement() {
    next();
    expect(p_lparen);
    const Value c = condition(expression());
    expect(p_rparen);
    const Block then_b = m_.block(), join = m_.block();
    Block else_b = join;
    // Look past the then-statement for an else: compile then first, decide after.
    const Block cond_block = m_.current();
    m_.at(then_b);
    statement();
    const Block then_end = m_.current();
    if (accept_id(s_else)) {
      else_b = m_.block();
      m_.at(else_b);
      statement();
      branch(join);
    }
    m_.at(then_end);
    branch(join);
    m_.at(cond_block);
    m_.cond_br(c, then_b, else_b);
    m_.at(join);
  }
  void for_statement() {
    next();
    expect(p_lparen);
    scopes_.emplace_back();
    if (!accept(p_semi)) statement();  // the init (a declaration or expression, with its ';')
    const Block head = m_.block(), body = m_.block(), step = m_.block(), exit = m_.block();
    branch(head);
    m_.at(head);
    if (!is_p(p_semi)) m_.cond_br(condition(expression()), body, exit);
    else m_.br(body);
    expect(p_semi);
    const size_t step_at = pos_;
    // Skip the step for now: it is compiled after the body.
    int depth = 0;
    while (!(depth == 0 && is_p(p_rparen))) {
      if (is_p(p_lparen)) ++depth;
      if (is_p(p_rparen)) --depth;
      next();
    }
    const size_t step_end = pos_;
    expect(p_rparen);
    m_.at(body);
    loops_.push_back({exit, step});
    statement();
    loops_.pop_back();
    branch(step);
    const size_t after = pos_;
    m_.at(step);
    pos_ = step_at;
    if (pos_ < step_end) expression();
    pos_ = after;
    m_.br(head);
    m_.at(exit);
    scopes_.pop_back();
  }
  void while_statement() {
    next();
    const Block head = m_.block(), body = m_.block(), exit = m_.block();
    branch(head);
    m_.at(head);
    expect(p_lparen);
    m_.cond_br(condition(expression()), body, exit);
    expect(p_rparen);
    m_.at(body);
    loops_.push_back({exit, head});
    statement();
    loops_.pop_back();
    branch(head);
    m_.at(exit);
  }
  void do_statement() {
    next();
    const Block body = m_.block(), test = m_.block(), exit = m_.block();
    branch(body);
    m_.at(body);
    loops_.push_back({exit, test});
    statement();
    loops_.pop_back();
    branch(test);
    m_.at(test);
    if (!accept_id(s_while)) fail("do without while");
    expect(p_lparen);
    m_.cond_br(condition(expression()), body, exit);
    expect(p_rparen);
    expect(p_semi);
    m_.at(exit);
  }
  void switch_statement() {
    next();
    expect(p_lparen);
    const RV v = value(expression());
    expect(p_rparen);
    const Value x = scalar_of(v, B::Uint);
    expect(p_lbrace);
    const Block dispatch = m_.current(), exit = m_.block();
    std::vector<std::pair<uint32_t, Block>> cases;
    Block def = exit;
    dead();
    loops_.push_back({exit, loops_.empty() ? exit : loops_.back().cont});
    scopes_.emplace_back();
    while (!accept(p_rbrace)) {
      if (is_id(s_case) || is_id(s_default)) {
        const Block b = m_.block();
        branch(b);  // fall through
        m_.at(b);
        while (is_id(s_case) || is_id(s_default)) {
          if (accept_id(s_default)) {
            def = b;
          } else {
            next();
            const RV k = value(conditional());
            uint64_t c = 0;
            if (k.c.empty() || !m_.constant_int(k.c[0], c)) fail("a case value that is not a constant");
            cases.emplace_back(static_cast<uint32_t>(c), b);
          }
          expect(p_colon);
        }
        continue;
      }
      statement();
    }
    scopes_.pop_back();
    loops_.pop_back();
    branch(exit);
    m_.at(dispatch);
    m_.switch_(x, def, cases);
    m_.at(exit);
  }
  void return_statement() {
    next();
    if (rets_.empty()) fail("return outside a function");
    if (!accept(p_semi)) {
      // The value first: a call in it is inlined and pushes onto rets_, which may move it.
      const RV v = value(expression());
      expect(p_semi);
      const Ret& r = rets_.back();
      if (r.t.b == B::Void) fail("a value returned from a void function");
      const RV c = convert(v, r.t);
      for (size_t k = 0; k < r.vars.size(); ++k) m_.write(r.vars[k], c.c[k]);
    }
    m_.br(rets_.back().exit);
    dead();
  }

  Value condition(const Ex& e) { return scalar_of(value(e), B::Bool); }

  // ---- expressions ----
  Ex expression() {
    Ex e = assignment();
    while (accept(p_comma)) e = assignment();
    return e;
  }
  Ex assignment() {
    Ex l = conditional();
    const Token& t = peek();
    if (t.kind != Tk::Punct) return l;
    const std::string& op = syms_.name(t.sym);
    if (op != "=" && op != "+=" && op != "-=" && op != "*=" && op != "/=" && op != "%=" && op != "<<=" && op != ">>=" && op != "&=" &&
        op != "|=" && op != "^=")
      return l;
    next();
    if (!l.is_lv) fail("an assignment to a value");
    const RV r = value(assignment());
    if (op == "=") {
      store(l.lv, r);
    } else {
      const RV cur = load(l.lv);
      store(l.lv, binary(op.substr(0, op.size() - 1), cur, r));
    }
    return l;
  }
  Ex conditional() {
    Ex c = binary_level(0);
    if (!accept(p_quest)) return c;
    // Both sides are evaluated (the translator's operands have no side effects) and selected.
    const RV a = value(assignment());
    expect(p_colon);
    const RV b = value(conditional());
    return rv(select(value(c), a, b));
  }
  // Binary operators by precedence: 0 ||, 1 &&, 2 |, 3 ^, 4 &, 5 == !=, 6 < > <= >=, 7 << >>, 8 + -, 9 * / %.
  int precedence(const Token& t) const {
    if (t.kind != Tk::Punct) return -1;
    const std::string& p = syms_.name(t.sym);
    if (p == "||") return 0;
    if (p == "&&") return 1;
    if (p == "|") return 2;
    if (p == "^") return 3;
    if (p == "&") return 4;
    if (p == "==" || p == "!=") return 5;
    if (p == "<" || p == ">" || p == "<=" || p == ">=") return 6;
    if (p == "<<" || p == ">>") return 7;
    if (p == "+" || p == "-") return 8;
    if (p == "*" || p == "/" || p == "%") return 9;
    return -1;
  }
  Ex binary_level(int min) {
    Ex l = unary();
    for (;;) {
      const int p = precedence(peek());
      if (p < min) return l;
      const std::string op = syms_.name(next().sym);
      const Ex r = binary_level(p + 1);
      l = rv(binary(op, value(l), value(r)));
    }
  }

  // The usual arithmetic conversions: the common base and width of two operands.
  void common(const RV& a, const RV& b, B& base, uint8_t& n) {
    const B x = a.t.b, y = b.t.b;
    if (!is_numeric(x) || !is_numeric(y)) fail("an operator on values that are not numbers");
    if (a.lit && !b.lit) base = is_float(x) && !is_float(y) ? B::Float : (y == B::Bool ? B::Int : y);
    else if (b.lit && !a.lit) base = is_float(y) && !is_float(x) ? B::Float : (x == B::Bool ? B::Int : x);
    else base = rank(x) >= rank(y) ? x : y;
    if (a.t.n == 1) n = b.t.n;
    else if (b.t.n == 1) n = a.t.n;
    else n = std::min(a.t.n, b.t.n);
  }
  RV binary(const std::string& op, const RV& a, const RV& b) {
    if (op == "&&" || op == "||") {
      // HLSL 2021 short-circuits these; the translator's operands have no side effects, so both
      // are evaluated (as DXC does at -O3 for such operands).
      const RV x = with_base(a, B::Bool), y = with_base(b, B::Bool);
      return lanes(x, y, B::Bool, [&](Value p, Value q) { return op == "&&" ? m_.and_(p, q) : m_.or_(p, q); });
    }
    if (op == "<<" || op == ">>") {
      B base = a.t.b == B::Bool ? B::Int : a.t.b;
      if (!is_int(base)) fail("a shift of a value not an integer");
      RV x = with_base(a, base);
      uint8_t n = a.t.n;
      if (b.t.n > n && a.t.n == 1) n = b.t.n;
      Ty vt = scalar(base, n);
      x = convert(x, vt);
      RV y = convert(with_base(b, base), vt);
      const uint32_t bits = base == B::Int64 || base == B::Uint64 ? 63 : 31;
      RV r;
      r.t = vt;
      for (size_t k = 0; k < x.c.size(); ++k) {
        const Value amt = m_.and_(y.c[k], m_.const_int(ir(base), bits));
        r.c.push_back(op == "<<" ? m_.shl(x.c[k], amt) : is_signed(base) ? m_.ashr(x.c[k], amt) : m_.lshr(x.c[k], amt));
      }
      return r;
    }
    B base;
    uint8_t n;
    common(a, b, base, n);
    const bool compare = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
    B work = base;
    if (!compare && work == B::Bool) work = B::Int;  // arithmetic and bitwise on bools: int
    const Ty wt = scalar(work, n);
    const RV x = convert(a, wt), y = convert(b, wt);
    RV r;
    r.t = scalar(compare ? B::Bool : work, n);
    r.lit = a.lit && b.lit;
    const bool fl = is_float(work), sg = is_signed(work);
    for (size_t k = 0; k < x.c.size(); ++k) {
      const Value p = x.c[k], q = y.c[k];
      Value v;
      if (compare) {
        Pred pr;
        if (fl) {
          pr = op == "==" ? Pred::FOeq : op == "!=" ? Pred::FUne : op == "<" ? Pred::FOlt : op == ">" ? Pred::FOgt : op == "<=" ? Pred::FOle : Pred::FOge;
        } else if (op == "==" || op == "!=") {
          pr = op == "==" ? Pred::IEq : Pred::INe;
        } else {
          pr = sg ? (op == "<" ? Pred::ISlt : op == ">" ? Pred::ISgt : op == "<=" ? Pred::ISle : Pred::ISge)
                  : (op == "<" ? Pred::IUlt : op == ">" ? Pred::IUgt : op == "<=" ? Pred::IUle : Pred::IUge);
        }
        v = m_.cmp(pr, p, q, kFastMath);
      } else if (op == "+") v = m_.bin(Bin::Add, p, q, fl ? kFastMath : 0);
      else if (op == "-") v = m_.bin(Bin::Sub, p, q, fl ? kFastMath : 0);
      else if (op == "*") v = m_.bin(Bin::Mul, p, q, fl ? kFastMath : 0);
      else if (op == "/") v = fl ? m_.bin(Bin::SDiv, p, q, kFastMath) : m_.bin(sg ? Bin::SDiv : Bin::UDiv, p, q);
      else if (op == "%") v = fl ? m_.bin(Bin::SRem, p, q, kFastMath) : m_.bin(sg ? Bin::SRem : Bin::URem, p, q);
      else if (op == "&" || op == "|" || op == "^") {
        if (fl) fail("a bitwise operator on floats");
        v = m_.bin(op == "&" ? Bin::And : op == "|" ? Bin::Or : Bin::Xor, p, q);
      } else {
        fail("operator " + op);
      }
      r.c.push_back(v);
    }
    return r;
  }
  template <typename F>
  RV lanes(const RV& a, const RV& b, B base, F&& f) {
    const uint8_t n = a.t.n == 1 ? b.t.n : b.t.n == 1 ? a.t.n : std::min(a.t.n, b.t.n);
    const RV x = convert(a, scalar(base, n)), y = convert(b, scalar(base, n));
    RV r;
    r.t = scalar(base, n);
    for (size_t k = 0; k < n; ++k) r.c.push_back(f(x.c[k], y.c[k]));
    return r;
  }
  RV select(const RV& c, const RV& a, const RV& b) {
    Ty t;
    if (a.t.b == B::Struct || a.t.arr) {
      if (!(a.t == b.t)) fail("a select of two aggregates of different types");
      t = a.t;
    } else {
      B base;
      uint8_t n;
      common(a, b, base, n);
      if (c.t.n > n && a.t.n == 1 && b.t.n == 1) n = c.t.n;
      t = scalar(base, n);
    }
    const RV x = convert(a, t), y = convert(b, t);
    const RV cc = with_base(c, B::Bool);
    RV r;
    r.t = t;
    for (size_t k = 0; k < x.c.size(); ++k) r.c.push_back(m_.select(cc.c.size() == 1 ? cc.c[0] : cc.c[k], x.c[k], y.c[k]));
    return r;
  }

  Ex unary() {
    const Token& t = peek();
    if (t.kind == Tk::Punct) {
      const std::string& p = syms_.name(t.sym);
      if (p == "-" || p == "+" || p == "!" || p == "~") {
        next();
        const RV v = value(unary());
        if (p == "+") return rv(v);
        if (p == "!") {
          const RV b = with_base(v, B::Bool);
          RV r;
          r.t = b.t;
          for (Value x : b.c) r.c.push_back(m_.xor_(x, m_.boolean(true)));
          return rv(r);
        }
        B base = v.t.b == B::Bool ? B::Int : v.t.b;
        const RV x = with_base(v, base);
        RV r;
        r.t = x.t;
        r.lit = v.lit;
        for (Value c : x.c) {
          if (p == "-") r.c.push_back(is_float(base) ? m_.bin(Bin::Sub, neg_zero(base), c, kFastMath) : m_.sub(zero(base), c));
          else r.c.push_back(m_.xor_(c, m_.const_int(ir(base), ~0ull)));
        }
        return rv(r);
      }
      if (p == "++" || p == "--") {
        next();
        Ex e = unary();
        if (!e.is_lv) fail("++ or -- of a value");
        store(e.lv, binary(p == "++" ? "+" : "-", load(e.lv), one()));
        return e;
      }
      // A cast: '(' type ')' unary.
      if (p == "(" && type_name(1) && is_p(p_rparen, 2)) {
        next();
        const Ty to = parse_type();
        expect(p_rparen);
        const RV v = value(unary());
        return rv(convert(v, to));
      }
    }
    return postfix(primary());
  }

  Ex postfix(Ex e) {
    for (;;) {
      if (accept(p_lbrack)) {
        const RV idx = value(expression());
        expect(p_rbrack);
        e = index(e, idx);
        continue;
      }
      if (is_p(p_dot)) {
        next();
        const uint32_t name = ident();
        if (is_p(p_lparen)) {
          e = method(e, name);
          continue;
        }
        e = member(e, name);
        continue;
      }
      const Token& t = peek();
      if (t.kind == Tk::Punct && (syms_.name(t.sym) == "++" || syms_.name(t.sym) == "--")) {
        const bool inc = syms_.name(t.sym) == "++";
        next();
        if (!e.is_lv) fail("++ or -- of a value");
        const RV old = load(e.lv);
        store(e.lv, binary(inc ? "+" : "-", old, one()));
        return rv(old);
      }
      return e;
    }
  }

  // Swizzles and struct members.
  Ex member(const Ex& e, uint32_t name) {
    const Ty t = e.is_lv ? e.lv.t : e.rv.t;
    const std::string& n = syms_.name(name);
    if (t.b == B::Struct && !t.arr) {
      const StructDef& sd = structs_[t.st];
      size_t at = 0;
      for (const auto& [mn, mt] : sd.members) {
        const size_t cn = comps(mt);
        if (mn == name) {
          Ex out;
          out.is_lv = e.is_lv;
          if (e.is_lv) {
            out.lv = e.lv;
            out.lv.t = mt;
            if (e.lv.kind == LV::Vars) out.lv.vars.assign(e.lv.vars.begin() + static_cast<std::ptrdiff_t>(at), e.lv.vars.begin() + static_cast<std::ptrdiff_t>(at + cn));
            else fail("a member of a struct in memory");
          } else {
            out.rv.t = mt;
            out.rv.c.assign(e.rv.c.begin() + static_cast<std::ptrdiff_t>(at), e.rv.c.begin() + static_cast<std::ptrdiff_t>(at + cn));
          }
          return out;
        }
        at += cn;
      }
      fail("no member " + n);
    }
    if ((is_numeric(t.b)) && !t.arr) {
      std::vector<size_t> pick;
      for (char ch : n) {
        const char* xs = "xyzw";
        const char* rs = "rgba";
        const char* p = std::strchr(xs, ch);
        size_t k = p ? static_cast<size_t>(p - xs) : 4;
        if (k == 4) {
          p = std::strchr(rs, ch);
          k = p ? static_cast<size_t>(p - rs) : 4;
        }
        if (k >= t.n) fail("swizzle ." + n);
        pick.push_back(k);
      }
      if (pick.empty() || pick.size() > 4) fail("swizzle ." + n);
      Ex out;
      out.is_lv = e.is_lv && e.lv.kind == LV::Vars;
      const Ty nt = scalar(t.b, static_cast<uint8_t>(pick.size()));
      if (out.is_lv) {
        out.lv.t = nt;
        for (size_t k : pick) out.lv.vars.push_back(e.lv.vars[k]);
      } else {
        const RV v = value(e);
        out.rv.t = nt;
        for (size_t k : pick) out.rv.c.push_back(v.c[k]);
      }
      return out;
    }
    fail("member ." + n + " of a value with none");
  }

  Ex index(const Ex& e, const RV& idx) {
    const Ty t = e.is_lv ? e.lv.t : e.rv.t;
    uint64_t k = 0;
    const bool constant = !idx.c.empty() && m_.constant_int(idx.c[0], k);
    if (t.b == B::Res) return texel(e, idx);
    if (t.b == B::HeapRef) fail("an index of a heap reference");
    if (t.arr) {
      const Ty et = element(t);
      const size_t cn = comps(et);
      if (e.is_lv && e.lv.kind == LV::Mem && e.lv.arr != kNone) {
        Ex out;
        out.is_lv = true;
        out.lv.kind = LV::Mem;
        out.lv.t = et;
        const Value i = scalar_of(idx, B::Uint);
        const Value at = et.n == 1 ? i : m_.mul(i, m_.u32(et.n));
        for (uint32_t j = 0; j < et.n; ++j)
          out.lv.ptrs.push_back(m_.gep(e.lv.arr_type, e.lv.arr, {m_.u32(0), j == 0 ? at : m_.add(at, m_.u32(j))}));
        return out;
      }
      if (!constant) fail("an array in registers indexed by a value");
      if (k >= t.arr) fail("an array index past its end");
      Ex out;
      out.is_lv = e.is_lv;
      if (e.is_lv) {
        out.lv.t = et;
        out.lv.vars.assign(e.lv.vars.begin() + static_cast<std::ptrdiff_t>(k * cn), e.lv.vars.begin() + static_cast<std::ptrdiff_t>(k * cn + cn));
      } else {
        out.rv.t = et;
        out.rv.c.assign(e.rv.c.begin() + static_cast<std::ptrdiff_t>(k * cn), e.rv.c.begin() + static_cast<std::ptrdiff_t>(k * cn + cn));
      }
      return out;
    }
    if (is_numeric(t.b) && t.n > 1) {
      if (constant) {
        if (k >= t.n) fail("a vector index past its end");
        Ex out;
        out.is_lv = e.is_lv && e.lv.kind == LV::Vars;
        if (out.is_lv) {
          out.lv.t = scalar(t.b);
          out.lv.vars = {e.lv.vars[k]};
        } else {
          const RV v = value(e);
          out.rv = make(t.b, {v.c[k]});
        }
        return out;
      }
      // A vector at a value: a place (a write selects into each component) or a chain of selects.
      if (e.is_lv && e.lv.kind == LV::Vars) {
        Ex out;
        out.is_lv = true;
        out.lv.kind = LV::Pick;
        out.lv.t = scalar(t.b);
        out.lv.vars = e.lv.vars;
        out.lv.pick = scalar_of(idx, B::Uint);
        return out;
      }
      const RV v = value(e);
      const Value i = scalar_of(idx, B::Uint);
      Value r = v.c[t.n - 1];
      for (int c = t.n - 2; c >= 0; --c) r = m_.select(m_.cmp(Pred::IEq, i, m_.u32(static_cast<uint32_t>(c))), v.c[static_cast<size_t>(c)], r);
      return rv(make(t.b, {r}));
    }
    fail("an index of a value that is not an array or vector");
  }
  // ---- primaries ----
  Ex primary() {
    const Token t = next();
    switch (t.kind) {
      case Tk::Int: {
        const bool wide = t.suffix & 2;
        RV r = make(wide ? (t.suffix & 1 ? B::Uint64 : B::Int64) : (t.suffix & 1 ? B::Uint : B::Int), {m_.const_int(wide ? m_.i64() : m_.i32(), t.bits)});
        r.lit = !(t.suffix & 1) && !wide;
        return rv(r);
      }
      case Tk::Float: {
        if (t.suffix == 2) return rv(make(B::Double, {m_.const_float(m_.f64(), t.bits)}));
        RV r = make(B::Float, {m_.f32c(static_cast<float>(float_of(t)))});
        r.lit = t.suffix == 4;
        return rv(r);
      }
      case Tk::Punct:
        if (t.sym == p_lparen) {
          Ex e = expression();
          expect(p_rparen);
          return e;
        }
        break;
      case Tk::Ident: {
        const uint32_t s = t.sym;
        if (s == s_true || s == s_false) return rv(make(B::Bool, {m_.boolean(s == s_true)}));
        if (s == s_heap || s == s_sheap) {
          expect(p_lbrack);
          const RV i = value(expression());
          expect(p_rbrack);
          RV r;
          r.t.b = B::HeapRef;
          r.c = {scalar_of(i, B::Uint)};
          r.non_uniform = i.non_uniform;
          r.sampler_heap = s == s_sheap;
          return rv(r);
        }
        if (is_p(p_lparen)) {
          // A constructor or a call.
          Ty ct;
          if (parse_type_name(syms_.name(s), ct)) return rv(construct(ct));
          return call(s);
        }
        if (Local* l = find_local(s)) {
          if (l->t.b == B::Res) {
            RV r;
            r.t = l->t;
            r.c = {l->handle};
            return rv(r);
          }
          Ex e;
          e.is_lv = true;
          e.lv = local_lv(*l);
          return e;
        }
        const auto g = globals_.find(s);
        if (g != globals_.end()) return global(g->second);
        fail("unknown name " + syms_.name(s));
      }
      default: break;
    }
    --pos_;
    fail("an expression");
  }

  RV construct(const Ty& t) {
    expect(p_lparen);
    std::vector<RV> args;
    while (!accept(p_rparen)) {
      args.push_back(value(assignment()));
      accept(p_comma);
    }
    if (args.size() == 1 && (args[0].t.n == 1 || t.n == 1 || args[0].t.n == t.n)) return convert(args[0], t);
    RV r;
    r.t = t;
    for (const RV& a : args) {
      for (Value c : a.c) r.c.push_back(conv(c, a.t.b, t.b));
    }
    if (r.c.size() != t.n) fail("a constructor with the wrong number of components");
    return r;
  }

  Ex global(Global& g) {
    if (g.kind == Global::Shared && g.ptr == kNone) {
      g.ptr = m_.global("am.shared." + syms_.name(g.name), m_.array(ir(g.t.b), g.t.arr), 3, 4);
    } else if (g.kind == Global::Constant && g.ptr == kNone) {
      const Type at = m_.array(ir(g.t.b), g.t.arr);
      g.ptr = m_.global("am.const." + syms_.name(g.name), at, 0, 4, m_.aggregate(at, g.init));
    }
    switch (g.kind) {
      case Global::Resource: {
        if (g.handle == kNone) {
          const Block keep = m_.current();
          m_.at(0);
          g.handle = m_.bind_raw_buffer(g.cls, g.space, g.reg);
          m_.at(keep);
        }
        RV r;
        r.t = g.t;
        r.c = {g.handle};
        return rv(r);
      }
      case Global::CbMember: {
        CBuffer& cb = cbuffers_[g.cb];
        if (cb.handle == kNone) {
          const Block keep = m_.current();
          m_.at(0);
          cb.handle = m_.bind_cbuffer(cb.space, cb.reg, cb.bytes);
          m_.at(keep);
        }
        if (g.t.arr) {
          // Indexed below: a cbuffer array reference.
          expect(p_lbrack);
          const RV i = value(expression());
          expect(p_rbrack);
          const Value row = m_.add(m_.u32(g.offset / 16), scalar_of(i, B::Uint));
          return rv(cb_row(cb.handle, row, element(g.t), 0));
        }
        return rv(cb_row(cb.handle, m_.u32(g.offset / 16), g.t, (g.offset % 16) / 4));
      }
      case Global::Shared:
      case Global::Constant: {
        // An array in memory: the element pointers come from its index (below).
        expect(p_lbrack);
        const RV i = value(expression());
        expect(p_rbrack);
        const Type at = m_.array(ir(g.t.b), g.t.arr);
        Ex e;
        e.is_lv = true;
        e.lv.kind = LV::Mem;
        e.lv.t = element(g.t);
        e.lv.ptrs = {m_.gep(at, g.ptr, {m_.u32(0), scalar_of(i, B::Uint)})};
        if (g.kind == Global::Constant) return rv(load(e.lv));
        return e;
      }
    }
    fail("a global");
  }
  RV cb_row(Value handle, Value row, const Ty& t, uint32_t first) {
    const Value r = m_.cbuffer_row(handle, row);
    RV out;
    out.t = t;
    for (uint32_t k = 0; k < t.n; ++k) out.c.push_back(conv(m_.extract(r, first + k), B::Uint, t.b == B::Float ? B::Uint : t.b));
    if (t.b == B::Float) {
      for (Value& c : out.c) c = m_.bitcast(c, m_.f32());
    }
    return out;
  }

  // A resource local's handle from its initializer (a heap reference, or another resource).
  Value resource_handle(const Ty& t, const RV& init) {
    if (init.t.b == B::Res) return init.c[0];
    if (init.t.b != B::HeapRef) fail("a resource initialized from a value");
    uint32_t p0 = 0, p1 = 0;
    props(t, p0, p1);
    return m_.heap_handle(init.c[0], init.sampler_heap, init.non_uniform, p0, p1);
  }
  // DXIL's resource properties: the kind (and 0x1000 for a UAV, 0x8000 for a comparison
  // sampler); for a typed view its element's component type and count (count << 8).
  void props(const Ty& t, uint32_t& p0, uint32_t& p1) {
    p1 = 0;
    switch (t.rk) {
      case RK::Raw: p0 = 11; return;
      case RK::RWRaw: p0 = 11 | 0x1000; return;
      case RK::Sampler: p0 = 14; return;
      case RK::SamplerCmp: p0 = 14 | 0x8000; return;
      default: break;
    }
    const TexShape s = shape(t.rk);
    p0 = s.kind | (s.uav ? 0x1000u : 0u);
    uint32_t ct = 0;
    if (is_float(t.elem)) ct = 9;
    else if (t.elem == B::Uint) ct = 5;
    else if (t.elem == B::Int) ct = 4;
    else fail("a texture of this element type");
    p1 = ct | (static_cast<uint32_t>(t.en) << 8);
  }

  // ---- calls ----
  std::vector<RV> args_until_paren() {
    std::vector<RV> args;
    while (!accept(p_rparen)) {
      args.push_back(value(assignment()));
      accept(p_comma);
    }
    return args;
  }
  std::vector<Ex> exprs_until_paren() {
    std::vector<Ex> args;
    while (!accept(p_rparen)) {
      args.push_back(assignment());
      accept(p_comma);
    }
    return args;
  }

  Ex call(uint32_t name) {
    expect(p_lparen);
    const auto f = fns_.find(name);
    if (f != fns_.end()) return rv(inline_call(*f->second));
    return rv(intrinsic(syms_.name(name)));
  }

  RV inline_call(Fn& f) {
    std::vector<Ex> args = exprs_until_paren();
    if (args.size() > f.params.size()) fail("too many arguments to " + syms_.name(f.name));
    // Arguments are values (in) or places (out, inout); defaults are evaluated in the caller.
    std::vector<RV> in(f.params.size());
    for (size_t k = 0; k < f.params.size(); ++k) {
      const Param& p = f.params[k];
      if (k < args.size()) {
        if (p.dir != 0 && !args[k].is_lv) fail("an out argument that is not a place");
        if (p.dir != 1) in[k] = value(args[k]);
      } else {
        if (p.def_begin == p.def_end) fail("too few arguments to " + syms_.name(f.name));
        const size_t keep = pos_;
        pos_ = p.def_begin;
        in[k] = value(assignment());
        pos_ = keep;
      }
    }
    std::deque<std::unordered_map<uint32_t, Local>> saved_scopes;
    saved_scopes.swap(scopes_);
    scopes_.emplace_back();
    std::vector<Local*> locals(f.params.size());
    for (size_t k = 0; k < f.params.size(); ++k) {
      const Param& p = f.params[k];
      if (p.t.b == B::Res) {
        Local& l = declare(p.name, p.t, false);
        l.handle = in[k].c[0];
        locals[k] = &l;
        continue;
      }
      Local& l = declare(p.name, p.t, false);
      locals[k] = &l;
      if (p.dir != 1) store(local_lv(l), in[k]);
    }
    Ret r;
    r.exit = m_.block();
    r.t = f.ret;
    if (f.ret.b != B::Void) {
      std::vector<B> tb;
      bases(f.ret, tb);
      for (B b : tb) r.vars.push_back(m_.variable(ir(b)));
    }
    rets_.push_back(r);
    Fn* outer = fn_;
    fn_ = &f;
    find_dynamic_arrays(f);
    std::vector<Loop> saved_loops;
    saved_loops.swap(loops_);
    const size_t keep = pos_;
    pos_ = f.body;
    block_body();
    pos_ = keep;
    loops_.swap(saved_loops);
    fn_ = outer;
    branch(r.exit);
    rets_.pop_back();
    m_.at(r.exit);
    // out and inout: copied back.
    std::vector<RV> outs(f.params.size());
    for (size_t k = 0; k < f.params.size(); ++k) {
      if (f.params[k].dir != 0 && f.params[k].t.b != B::Res) outs[k] = load(local_lv(*locals[k]));
    }
    scopes_.swap(saved_scopes);
    for (size_t k = 0; k < f.params.size() && k < args.size(); ++k) {
      if (f.params[k].dir != 0 && f.params[k].t.b != B::Res) store(args[k].lv, outs[k]);
    }
    RV result;
    result.t = f.ret;
    for (uint32_t v : r.vars) result.c.push_back(m_.read(v));
    return result;
  }

  // ---- intrinsics ----
  Value dx1(uint32_t op, const char* klass, B b, Value x, Attr a = Attr::ReadNone) {
    const Type t = ir(b);
    return m_.dx_call(op, klass, t, t, {t}, a, {x});
  }
  RV per(const RV& x, B b, uint32_t op, const char* klass) {
    const RV v = with_base(x, b);
    RV r;
    r.t = v.t;
    for (Value c : v.c) r.c.push_back(dx1(op, klass, b, c));
    return r;
  }
  RV per2(const RV& x, const RV& y, B b, uint32_t op, const char* klass) {
    return lanes(x, y, b, [&](Value p, Value q) {
      const Type t = ir(b);
      return m_.dx_call(op, klass, t, t, {t, t}, Attr::ReadNone, {p, q});
    });
  }
  // The common base of three operands (literals take the others' type).
  B common3(const RV& a, const RV& b, const RV& c) {
    B ab;
    uint8_t n;
    common(a, b, ab, n);
    RV t = a;
    t.t.b = ab;
    t.lit = a.lit && b.lit;
    B r;
    common(t, c, r, n);
    return r == B::Bool ? B::Int : r;
  }
  B float_base(const RV& x) const { return x.t.b == B::Double ? B::Double : B::Float; }
  B int_base(const RV& a, const RV& b) {
    B base;
    uint8_t n;
    common(a, b, base, n);
    return base == B::Bool ? B::Int : base;
  }
  RV fmap(const RV& x, const std::function<Value(Value)>& f) {
    const RV v = with_base(x, float_base(x));
    RV r;
    r.t = v.t;
    for (Value c : v.c) r.c.push_back(f(c));
    return r;
  }
  RV bitcast_to(const RV& x, B to) {
    RV r;
    r.t = scalar(to, x.t.n);
    const Type t = ir(to);
    for (Value c : x.c) {
      if (x.t.b == B::Bool) c = conv(c, B::Bool, B::Uint);
      r.c.push_back(m_.type_of(c) == t ? c : m_.bitcast(c, t));
    }
    return r;
  }
  Value wave_lane(uint32_t op, const char* klass, Value x, std::vector<Value> extra, std::vector<Type> extra_t) {
    const Type t = m_.type_of(x);
    std::vector<Type> params{t};
    params.insert(params.end(), extra_t.begin(), extra_t.end());
    std::vector<Value> args{x};
    args.insert(args.end(), extra.begin(), extra.end());
    return m_.dx_call(op, klass, t, t, params, Attr::None, args);
  }
  // Values the wave operations take: bools and halves go as i32 / float as DXC sends them.
  RV wave_each(const RV& x, const std::function<Value(Value)>& f) {
    RV v = x;
    if (v.t.b == B::Bool) v = with_base(v, B::Uint);
    RV r;
    r.t = v.t;
    for (Value c : v.c) r.c.push_back(f(c));
    if (x.t.b == B::Bool) r = with_base(r, B::Bool);
    return r;
  }

  RV intrinsic(const std::string& n) {
    if (n == s_nonuniform_name()) {
      std::vector<RV> a = args_until_paren();
      if (a.size() != 1) fail("NonUniformResourceIndex arguments");
      a[0].non_uniform = true;
      return a[0];
    }
    if (n == "InterlockedAdd" || n == "InterlockedMin" || n == "InterlockedMax" || n == "InterlockedAnd" || n == "InterlockedOr" ||
        n == "InterlockedXor" || n == "InterlockedExchange" || n == "InterlockedCompareExchange") {
      return shared_atomic(n);
    }
    std::vector<RV> a;
    if (n == "asuint") {
      // asuint(double, out uint lo, out uint hi): DXIL's splitDouble; asuint(x) below.
      std::vector<Ex> e = exprs_until_paren();
      if (e.size() == 3) {
        if (!e[1].is_lv || !e[2].is_lv) fail("asuint: outputs that are not places");
        const Value d = scalar_of(value(e[0]), B::Double);
        const Type st = m_.named_struct("dx.types.splitdouble", {m_.i32(), m_.i32()});
        const Value r = m_.dx_call(102, "splitDouble", m_.f64(), st, {m_.f64()}, Attr::ReadNone, {d});
        store(e[1].lv, make(B::Uint, {m_.extract(r, 0)}));
        store(e[2].lv, make(B::Uint, {m_.extract(r, 1)}));
        RV none;
        none.t.b = B::Void;
        return none;
      }
      for (const Ex& x : e) a.push_back(value(x));
    } else {
      a = args_until_paren();
    }
    const auto need = [&](size_t k) {
      if (a.size() != k) fail(n + ": " + std::to_string(a.size()) + " arguments");
    };
    // Bit reinterpretation.
    if (n == "asuint" || n == "asint" || n == "asfloat") {
      need(1);
      const B to = n == "asuint" ? B::Uint : n == "asint" ? B::Int : B::Float;
      if (a[0].t.b == B::Double || a[0].t.b == B::Int64 || a[0].t.b == B::Uint64) fail(n + " of a 64-bit value");
      return bitcast_to(a[0].t.b == B::Half ? with_base(a[0], B::Float) : a[0], to);
    }
    if (n == "asdouble") {
      need(2);
      const Value lo = scalar_of(a[0], B::Uint), hi = scalar_of(a[1], B::Uint);
      return make(B::Double, {m_.dx_call(101, "makeDouble", m_.f64(), m_.f64(), {m_.i32(), m_.i32()}, Attr::ReadNone, {lo, hi})});
    }
    if (n == "select") {
      need(3);
      return select(a[0], a[1], a[2]);
    }
    if (n == "dot") {
      // Floats: DXIL's dot2/dot3/dot4; integers: products added in order.
      need(2);
      const B b = rank(a[0].t.b) >= rank(a[1].t.b) ? a[0].t.b : a[1].t.b;
      const B base = is_float(b) ? (b == B::Double ? B::Double : B::Float) : (b == B::Bool ? B::Int : b);
      const size_t w = std::max(a[0].t.n, a[1].t.n);
      const RV x = convert(a[0], scalar(base, static_cast<uint8_t>(w))), y = convert(a[1], scalar(base, static_cast<uint8_t>(w)));
      if (base == B::Float && w >= 2) {
        std::vector<Value> args(x.c.begin(), x.c.end());
        args.insert(args.end(), y.c.begin(), y.c.end());
        const Type f = m_.f32();
        const std::vector<Type> params(2 * w, f);
        const char* klass = w == 2 ? "dot2" : w == 3 ? "dot3" : "dot4";
        return make(B::Float, {m_.dx_call(static_cast<uint32_t>(52 + w), klass, f, f, params, Attr::ReadNone, args)});
      }
      const uint32_t fm = is_float(base) ? kFastMath : 0;
      Value acc = m_.bin(Bin::Mul, x.c[0], y.c[0], fm);
      for (size_t k = 1; k < w; ++k)
        acc = m_.bin(Bin::Add, acc, m_.bin(Bin::Mul, x.c[k], y.c[k], fm), fm);
      return make(base, {acc});
    }
    if (n == "and" || n == "or") {
      need(2);
      return lanes(with_base(a[0], B::Bool), with_base(a[1], B::Bool), B::Bool,
                   [&](Value p, Value q) { return n == "and" ? m_.and_(p, q) : m_.or_(p, q); });
    }
    if (n == "all" || n == "any") {
      need(1);
      const RV b = with_base(a[0], B::Bool);
      Value r = b.c[0];
      for (size_t k = 1; k < b.c.size(); ++k) r = n == "all" ? m_.and_(r, b.c[k]) : m_.or_(r, b.c[k]);
      return make(B::Bool, {r});
    }
    // Floats.
    struct F1 {
      const char* name;
      uint32_t op;
    };
    static const F1 f1[] = {{"saturate", 7},  {"cos", 12},       {"sin", 13},  {"exp2", 21},  {"frac", 22}, {"log2", 23},
                            {"sqrt", 24},     {"rsqrt", 25},     {"round", 26}, {"floor", 27}, {"ceil", 28}, {"trunc", 29}};
    for (const F1& x : f1) {
      if (n == x.name) {
        need(1);
        return per(a[0], B::Float, x.op, "unary");
      }
    }
    if (n == "abs") {
      need(1);
      if (is_float(a[0].t.b)) return per(a[0], float_base(a[0]), 6, "unary");
      const B b = a[0].t.b == B::Bool ? B::Int : a[0].t.b;
      const RV x = with_base(a[0], b);
      RV r;
      r.t = x.t;
      for (Value c : x.c) r.c.push_back(m_.dx_call(37, "binary", ir(b), ir(b), {ir(b), ir(b)}, Attr::ReadNone, {c, m_.sub(zero(b), c)}));
      return r;
    }
    if (n == "rcp") {
      need(1);
      return fmap(a[0], [&](Value c) { return m_.bin(Bin::SDiv, m_.f32c(1.0f), c, kFastMath); });
    }
    if (n == "isnan" || n == "isinf" || n == "isfinite") {
      need(1);
      const uint32_t op = n == "isnan" ? 8 : n == "isinf" ? 9 : 10;
      const RV v = with_base(a[0], B::Float);
      RV r;
      r.t = scalar(B::Bool, v.t.n);
      for (Value c : v.c) r.c.push_back(m_.dx_call(op, "isSpecialFloat", m_.f32(), m_.i1(), {m_.f32()}, Attr::ReadNone, {c}));
      return r;
    }
    if (n == "min" || n == "max") {
      need(2);
      const B b = int_base(a[0], a[1]);
      if (is_float(b)) return per2(a[0], a[1], b == B::Double ? B::Double : B::Float, n == "min" ? 36 : 35, "binary");
      const bool sg = is_signed(b);
      return per2(a[0], a[1], b, n == "min" ? (sg ? 38 : 40) : (sg ? 37 : 39), "binary");
    }
    if (n == "clamp") {
      need(3);
      B bb = common3(a[0], a[1], a[2]);
      if (bb == B::Half) bb = B::Float;
      const bool sg = is_signed(bb), fl = is_float(bb);
      const RV lo = per2(a[0], a[1], bb, fl ? 35 : sg ? 37 : 39, "binary");
      return per2(lo, a[2], bb, fl ? 36 : sg ? 38 : 40, "binary");
    }
    if (n == "mad" || n == "fma") {
      need(3);
      B b = common3(a[0], a[1], a[2]);
      if (b == B::Half) b = B::Float;
      const uint8_t w = std::max({a[0].t.n, a[1].t.n, a[2].t.n});
      const Ty t = scalar(b, w);
      const RV x = convert(a[0], t), y = convert(a[1], t), z = convert(a[2], t);
      const uint32_t op = is_float(b) ? (n == "fma" ? 47 : 46) : is_signed(b) ? 48 : 49;
      RV r;
      r.t = t;
      for (size_t k = 0; k < w; ++k) {
        r.c.push_back(m_.dx_call(op, "tertiary", ir(b), ir(b), {ir(b), ir(b), ir(b)}, Attr::ReadNone, {x.c[k], y.c[k], z.c[k]}));
      }
      return r;
    }
    if (n == "ldexp") {
      need(2);
      const RV x = with_base(a[0], B::Float), e = with_base(a[1], B::Float);
      return lanes(x, e, B::Float, [&](Value p, Value q) { return m_.bin(Bin::Mul, p, dx1(21, "unary", B::Float, q), kFastMath); });
    }
    if (n == "f32tof16") {
      need(1);
      const RV v = with_base(a[0], B::Float);
      RV r;
      r.t = scalar(B::Uint, v.t.n);
      for (Value c : v.c) r.c.push_back(m_.dx_call(130, "legacyF32ToF16", kNone, m_.i32(), {m_.f32()}, Attr::ReadNone, {c}));
      return r;
    }
    if (n == "f16tof32") {
      need(1);
      const RV v = with_base(a[0], B::Uint);
      RV r;
      r.t = scalar(B::Float, v.t.n);
      for (Value c : v.c) r.c.push_back(m_.dx_call(131, "legacyF16ToF32", kNone, m_.f32(), {m_.i32()}, Attr::ReadNone, {c}));
      return r;
    }
    // Bits.
    if (n == "reversebits") {
      need(1);
      return per(a[0], B::Uint, 30, "unary");
    }
    if (n == "countbits" || n == "firstbitlow" || n == "firstbithigh") {
      need(1);
      const B b = a[0].t.b == B::Uint64 || a[0].t.b == B::Int64 ? a[0].t.b : (a[0].t.b == B::Int ? B::Int : B::Uint);
      const RV v = with_base(a[0], b);
      const bool sh = n == "firstbithigh" && is_signed(b);
      const uint32_t op = n == "countbits" ? 31 : n == "firstbitlow" ? 32 : sh ? 34 : 33;
      RV r;
      r.t = scalar(n == "countbits" ? B::Uint : (sh ? B::Int : B::Uint), v.t.n);
      const uint32_t top = b == B::Uint64 || b == B::Int64 ? 63 : 31;
      for (Value c : v.c) {
        Value x = m_.dx_call(op, "unaryBits", ir(b), m_.i32(), {ir(b)}, Attr::ReadNone, {c});
        if (n == "firstbithigh") {  // DXIL counts from the top: HLSL from the bottom, -1 kept
          x = m_.select(m_.cmp(Pred::IEq, x, m_.u32(0xFFFFFFFFu)), m_.u32(0xFFFFFFFFu), m_.sub(m_.u32(top), x));
        }
        r.c.push_back(x);
      }
      return r;
    }
    // Waves.
    if (n == "WaveReadLaneAt") {
      need(2);
      const Value lane = scalar_of(a[1], B::Uint);
      return wave_each(a[0], [&](Value c) { return wave_lane(117, "waveReadLaneAt", c, {lane}, {m_.i32()}); });
    }
    if (n == "WaveReadLaneFirst") {
      need(1);
      return wave_each(a[0], [&](Value c) { return wave_lane(118, "waveReadLaneFirst", c, {}, {}); });
    }
    if (n == "QuadReadAcrossX" || n == "QuadReadAcrossY" || n == "QuadReadAcrossDiagonal") {
      need(1);
      const uint8_t k = n == "QuadReadAcrossX" ? 0 : n == "QuadReadAcrossY" ? 1 : 2;
      return wave_each(a[0], [&](Value c) { return wave_lane(123, "quadOp", c, {m_.u8(k)}, {m_.i8()}); });
    }
    if (n == "QuadReadLaneAt") {
      need(2);
      const Value lane = scalar_of(a[1], B::Uint);
      return wave_each(a[0], [&](Value c) { return wave_lane(122, "quadReadLaneAt", c, {lane}, {m_.i32()}); });
    }
    if (n == "WaveActiveBallot") {
      need(1);
      const Type four = m_.named_struct("dx.types.fouri32", {m_.i32(), m_.i32(), m_.i32(), m_.i32()});
      const Value r = m_.dx_call(116, "waveActiveBallot", kNone, four, {m_.i1()}, Attr::None, {scalar_of(a[0], B::Bool)});
      return make(B::Uint, {m_.extract(r, 0), m_.extract(r, 1), m_.extract(r, 2), m_.extract(r, 3)});
    }
    if (n == "WaveIsFirstLane") {
      need(0);
      return make(B::Bool, {m_.dx_call(110, "waveIsFirstLane", kNone, m_.i1(), {}, Attr::None, {})});
    }
    if (n == "WaveGetLaneIndex") {
      need(0);
      return make(B::Uint, {m_.dx_call(111, "waveGetLaneIndex", kNone, m_.i32(), {}, Attr::ReadOnly, {})});
    }
    if (n == "WaveGetLaneCount") {
      need(0);
      return make(B::Uint, {m_.dx_call(112, "waveGetLaneCount", kNone, m_.i32(), {}, Attr::ReadNone, {})});
    }
    if (n == "WaveActiveAnyTrue" || n == "WaveActiveAllTrue") {
      need(1);
      const bool any = n == "WaveActiveAnyTrue";
      return make(B::Bool, {m_.dx_call(any ? 113 : 114, any ? "waveAnyTrue" : "waveAllTrue", kNone, m_.i1(), {m_.i1()}, Attr::None,
                                       {scalar_of(a[0], B::Bool)})});
    }
    if (n == "WaveActiveCountBits" || n == "WavePrefixCountBits") {
      need(1);
      const bool all = n == "WaveActiveCountBits";
      return make(B::Uint, {m_.dx_call(all ? 135 : 136, all ? "waveAllOp" : "wavePrefixOp", kNone, m_.i32(), {m_.i1()}, Attr::None,
                                       {scalar_of(a[0], B::Bool)})});
    }
    // Barriers: DXIL's modes (1 group sync, 2 UAV fence, 4 global UAV fence, 8 groupshared fence).
    if (n == "GroupMemoryBarrierWithGroupSync" || n == "GroupMemoryBarrier" || n == "AllMemoryBarrierWithGroupSync" ||
        n == "AllMemoryBarrier" || n == "DeviceMemoryBarrierWithGroupSync" || n == "DeviceMemoryBarrier") {
      need(0);
      uint32_t mode = n.find("Group") == 0 ? 8u : n.find("All") == 0 ? 10u : 2u;
      if (n.find("WithGroupSync") != std::string::npos) mode |= 1u;
      m_.dx_call(80, "barrier", kNone, m_.void_type(), {m_.i32()}, Attr::NoDuplicate, {m_.u32(mode)});
      return RV{};
    }
    fail("intrinsic " + n + " (not in the subset)");
  }
  std::string s_nonuniform_name() const { return syms_.name(s_nonuniform); }

  // Atomics on groupshared memory: InterlockedOp(lds[i], value[, original]).
  RV shared_atomic(const std::string& n) {
    std::vector<Ex> a = exprs_until_paren();
    if (a.size() >= 2 && a[0].is_lv && a[0].lv.kind == LV::Texel) return texel_atomic(n, a);
    if (a.size() < 2 || !a[0].is_lv || a[0].lv.kind != LV::Mem || a[0].lv.ptrs.size() != 1) fail(n + " on something not groupshared");
    const Value ptr = a[0].lv.ptrs[0];
    const B b = a[0].lv.t.b;
    Value old;
    size_t out_at;
    if (n == "InterlockedCompareExchange") {
      if (a.size() != 4) fail(n + " arguments");
      old = m_.cmpxchg(ptr, scalar_of(value(a[1]), b), scalar_of(value(a[2]), b));
      out_at = 3;
    } else {
      const Value x = scalar_of(value(a[1]), b);
      Rmw op = Rmw::Add;
      const bool sg = is_signed(b);
      if (n == "InterlockedMin") op = sg ? Rmw::Min : Rmw::UMin;
      else if (n == "InterlockedMax") op = sg ? Rmw::Max : Rmw::UMax;
      else if (n == "InterlockedAnd") op = Rmw::And;
      else if (n == "InterlockedOr") op = Rmw::Or;
      else if (n == "InterlockedXor") op = Rmw::Xor;
      else if (n == "InterlockedExchange") op = Rmw::Xchg;
      old = m_.atomic_rmw(op, ptr, x);
      out_at = 2;
    }
    if (a.size() > out_at) {
      if (!a[out_at].is_lv) fail(n + "'s original value into a value");
      store(a[out_at].lv, make(b, {old}));
    }
    return RV{};
  }

  // ---- resource methods ----
  Ex method(const Ex& e, uint32_t name) {
    const RV obj = value(e);
    if (obj.t.b != B::Res) fail("a method of a value that is not a resource");
    const std::string n = syms_.name(name);
    expect(p_lparen);
    if (obj.t.rk == RK::Raw || obj.t.rk == RK::RWRaw) return rv(raw_method(obj.c[0], obj.t.rk == RK::RWRaw, n));
    if (obj.t.rk == RK::Sampler || obj.t.rk == RK::SamplerCmp) fail("method " + n + " of a sampler");
    return rv(texture_method(obj, n));
  }
  RV raw_method(Value h, bool writable, const std::string& n) {
    if (n == "Load" || n == "Load2" || n == "Load3" || n == "Load4") {
      std::vector<RV> a = args_until_paren();
      if (a.size() != 1) fail(n + " arguments");
      const uint32_t count = n == "Load" ? 1 : static_cast<uint32_t>(n[4] - '0');
      const Value r = m_.raw_load(m_.i32(), h, scalar_of(a[0], B::Uint), (1u << count) - 1u, 4);
      RV out;
      out.t = scalar(B::Uint, static_cast<uint8_t>(count));
      for (uint32_t k = 0; k < count; ++k) out.c.push_back(m_.extract(r, k));
      return out;
    }
    if (!writable) fail(n + " of a read-only buffer");
    if (n == "Store" || n == "Store2" || n == "Store3" || n == "Store4") {
      std::vector<RV> a = args_until_paren();
      if (a.size() != 2) fail(n + " arguments");
      const uint8_t count = n == "Store" ? 1 : static_cast<uint8_t>(n[5] - '0');
      const RV v = convert(a[1], scalar(B::Uint, count));
      m_.raw_store(h, scalar_of(a[0], B::Uint), v.c, 4);
      return RV{};
    }
    if (n.compare(0, 11, "Interlocked") == 0) {
      std::vector<Ex> a = exprs_until_paren();
      const Type four[] = {m_.handle_type(), m_.i32(), m_.i32(), m_.i32(), m_.i32(), m_.i32()};
      if (a.size() < 2) fail(n + " arguments");
      const Value at = scalar_of(value(a[0]), B::Uint);
      Value old;
      size_t out_at;
      if (n == "InterlockedCompareExchange") {
        if (a.size() != 4) fail(n + " arguments");
        old = m_.dx_call(79, "atomicCompareExchange", m_.i32(), m_.i32(), {four[0], four[1], four[2], four[3], four[4], four[5]},
                         Attr::None, {h, at, m_.undef(m_.i32()), m_.undef(m_.i32()), scalar_of(value(a[1]), B::Uint), scalar_of(value(a[2]), B::Uint)});
        out_at = 3;
      } else {
        const RV x = value(a[1]);
        const bool sg = x.t.b == B::Int;
        uint32_t op = 0;  // DXIL's atomic ops: add, and, or, xor, imin, imax, umin, umax, exchange
        if (n == "InterlockedAdd") op = 0;
        else if (n == "InterlockedAnd") op = 1;
        else if (n == "InterlockedOr") op = 2;
        else if (n == "InterlockedXor") op = 3;
        else if (n == "InterlockedMin") op = sg ? 4 : 6;
        else if (n == "InterlockedMax") op = sg ? 5 : 7;
        else if (n == "InterlockedExchange") op = 8;
        else fail(n);
        old = m_.dx_call(78, "atomicBinOp", m_.i32(), m_.i32(), {four[0], four[1], four[2], four[3], four[4], four[5]}, Attr::None,
                         {h, m_.u32(op), at, m_.undef(m_.i32()), m_.undef(m_.i32()), scalar_of(x, B::Uint)});
        out_at = 2;
      }
      if (a.size() > out_at) {
        if (!a[out_at].is_lv) fail(n + "'s original value into a value");
        store(a[out_at].lv, make(a[out_at].lv.t.b == B::Int ? B::Int : B::Uint, {old}));
      }
      return RV{};
    }
    fail("buffer method " + n);
  }

  // ---- textures ----
  // A texture kind's shape: DXIL's kind number; coordinates a sample takes (with the array
  // slice), texel coordinates a load takes, offsets, gradient components; sizes GetDimensions
  // returns before the level count.
  struct TexShape {
    uint32_t kind;
    uint8_t coords, texel, offsets, grads, dims;
    bool ms, uav;
  };
  TexShape shape(RK k) const {
    switch (k) {
      case RK::Tex1D: return {1, 1, 1, 1, 1, 1, false, false};
      case RK::RWTex1D: return {1, 1, 1, 1, 1, 1, false, true};
      case RK::Tex1DArr: return {6, 2, 2, 1, 1, 2, false, false};
      case RK::RWTex1DArr: return {6, 2, 2, 1, 1, 2, false, true};
      case RK::Tex2D: return {2, 2, 2, 2, 2, 2, false, false};
      case RK::RWTex2D: return {2, 2, 2, 2, 2, 2, false, true};
      case RK::Tex2DArr: return {7, 3, 3, 2, 2, 3, false, false};
      case RK::RWTex2DArr: return {7, 3, 3, 2, 2, 3, false, true};
      case RK::Tex2DMS: return {3, 2, 2, 2, 2, 2, true, false};
      case RK::Tex2DMSArr: return {8, 3, 3, 2, 2, 3, true, false};
      case RK::Tex3D: return {4, 3, 3, 3, 3, 3, false, false};
      case RK::RWTex3D: return {4, 3, 3, 3, 3, 3, false, true};
      case RK::TexCube: return {5, 3, 0, 0, 3, 2, false, false};
      case RK::TexCubeArr: return {9, 4, 0, 0, 3, 3, false, false};
      default: fail("a resource that is not a texture");
    }
  }
  // The dx.op overload of a texture's element (f32, or i32 for integer views).
  Type overload_of(B elem) const { return is_float(elem) ? m_.f32() : m_.i32(); }
  Type res_ret(Type o) { return m_.named_struct(o == m_.f32() ? "dx.types.ResRet.f32" : "dx.types.ResRet.i32", {o, o, o, o, m_.i32()}); }
  // The first `n` values of a ResRet, as the view's element type.
  RV from_ret(Value r, B elem, uint8_t n) {
    RV out;
    out.t = scalar(is_float(elem) ? B::Float : elem, n);
    for (uint8_t k = 0; k < n; ++k) out.c.push_back(m_.extract(r, k));
    return out;
  }
  // `n` components of `v` as `b`, then undef up to `total` (DXIL's unused coordinates).
  std::vector<Value> parts(const RV& v, B b, size_t n, size_t total) {
    const RV x = with_base(v, b);
    if (x.c.size() < n) fail("a coordinate with too few components");
    std::vector<Value> out(x.c.begin(), x.c.begin() + static_cast<std::ptrdiff_t>(n));
    while (out.size() < total) out.push_back(m_.undef(ir(b)));
    return out;
  }
  // Offsets: the given ones, or 0 for each the shape takes (as DXC writes them); undef past it.
  std::vector<Value> offsets(const RV* given, const TexShape& s, size_t total) {
    if (given) return parts(*given, B::Int, s.offsets, total);
    std::vector<Value> out;
    for (size_t k = 0; k < total; ++k) out.push_back(k < s.offsets ? m_.u32(0) : m_.undef(m_.i32()));
    return out;
  }
  static void append(std::vector<Value>& to, const std::vector<Value>& v) { to.insert(to.end(), v.begin(), v.end()); }
  Value sampler_of(const RV& s) {
    if (s.t.b != B::Res || (s.t.rk != RK::Sampler && s.t.rk != RK::SamplerCmp)) fail("a sample without a sampler");
    return s.c[0];
  }

  RV texture_method(const RV& obj, const std::string& n) {
    const TexShape s = shape(obj.t.rk);
    const Value h = obj.c[0];
    const B elem = obj.t.elem;
    const Type o = overload_of(elem);
    const Type f = m_.f32(), i = m_.i32(), hd = m_.handle_type();
    if (n == "GetDimensions") {
      // (mip, sizes..., levels) for a view with levels; (sizes...) for a UAV or an MS texture.
      std::vector<Ex> a = exprs_until_paren();
      const bool with_mip = a.size() == static_cast<size_t>(s.dims) + 2u;
      if (!with_mip && a.size() != s.dims) fail("GetDimensions arguments");
      const Value mip = with_mip ? scalar_of(value(a[0]), B::Uint) : m_.undef(i);
      const Type dt = m_.named_struct("dx.types.Dimensions", {i, i, i, i});
      const Value d = m_.dx_call(72, "getDimensions", kNone, dt, {hd, i}, Attr::ReadOnly, {h, mip});
      const size_t first = with_mip ? 1 : 0;
      for (size_t k = 0; k < s.dims; ++k) {
        if (!a[first + k].is_lv) fail("GetDimensions into a value");
        store(a[first + k].lv, make(B::Uint, {m_.extract(d, static_cast<uint32_t>(k))}));
      }
      if (with_mip) {
        if (!a.back().is_lv) fail("GetDimensions into a value");
        store(a.back().lv, make(B::Uint, {m_.extract(d, 3)}));
      }
      return RV{};
    }
    std::vector<RV> a = args_until_paren();
    if (n == "Load") {
      // Load(coordinates with the level last) or, multisampled, Load(coordinates, sample).
      if (a.empty()) fail("Load arguments");
      Value mip;
      std::vector<Value> c;
      if (s.ms) {
        if (a.size() != 2) fail("Load of a multisampled texture: arguments");
        c = parts(a[0], B::Int, s.texel, 3);
        mip = scalar_of(a[1], B::Int);
      } else if (s.uav) {
        c = parts(a[0], B::Int, s.texel, 3);
        mip = m_.undef(i);
        if (obj.t.en > 1) m_.add_flags(1ull << 13);
      } else {
        const RV x = with_base(a[0], B::Int);
        if (x.c.size() != static_cast<size_t>(s.texel) + 1u) fail("Load coordinates");
        mip = x.c.back();
        c = parts(x, B::Int, s.texel, 3);
      }
      std::vector<Value> args{h, mip};
      append(args, c);
      append(args, {m_.undef(i), m_.undef(i), m_.undef(i)});
      const Value r = m_.dx_call(66, "textureLoad", o, res_ret(o), {hd, i, i, i, i, i, i, i}, Attr::ReadOnly, args);
      return from_ret(r, elem, obj.t.en);
    }
    if (n == "SampleLevel" || n == "SampleGrad" || n == "SampleCmpLevelZero") {
      const bool grad = n == "SampleGrad", cmp = n == "SampleCmpLevelZero";
      const size_t fixed = grad ? 4 : 3;  // sampler, coordinate, then lod / ddx+ddy / compare
      if (a.size() < fixed || a.size() > fixed + (grad ? 2u : 1u)) fail(n + " arguments");
      std::vector<Value> args{h, sampler_of(a[0])};
      append(args, parts(a[1], B::Float, s.coords, 4));
      append(args, offsets(a.size() > fixed ? &a[fixed] : nullptr, s, 3));
      std::vector<Type> params{hd, hd, f, f, f, f, i, i, i};
      if (grad) {
        append(args, parts(a[2], B::Float, s.grads, 3));
        append(args, parts(a[3], B::Float, s.grads, 3));
        const Value clamp = a.size() > 5 ? scalar_of(a[5], B::Float) : m_.undef(f);
        args.push_back(clamp);
        params.insert(params.end(), {f, f, f, f, f, f, f});
      } else {
        args.push_back(scalar_of(a[2], B::Float));
        params.push_back(f);
      }
      const uint32_t op = grad ? 63 : cmp ? 65 : 62;
      const char* klass = grad ? "sampleGrad" : cmp ? "sampleCmpLevelZero" : "sampleLevel";
      const Type ov = cmp ? f : o;
      const Value r = m_.dx_call(op, klass, ov, res_ret(ov), params, Attr::ReadOnly, args);
      return cmp ? from_ret(r, B::Float, 1) : from_ret(r, elem, obj.t.en);
    }
    if (n == "GatherRed" || n == "GatherGreen" || n == "GatherBlue" || n == "GatherAlpha" || n == "Gather") {
      if (a.size() < 2 || a.size() > 3) fail(n + " arguments");
      const uint32_t channel = n == "GatherGreen" ? 1 : n == "GatherBlue" ? 2 : n == "GatherAlpha" ? 3 : 0;
      std::vector<Value> args{h, sampler_of(a[0])};
      append(args, parts(a[1], B::Float, s.coords, 4));
      append(args, offsets(a.size() > 2 ? &a[2] : nullptr, s, 2));
      args.push_back(m_.u32(channel));
      const Value r = m_.dx_call(73, "textureGather", o, res_ret(o), {hd, hd, f, f, f, f, i, i, i}, Attr::ReadOnly, args);
      return from_ret(r, elem, 4);
    }
    fail("texture method " + n);
  }

  // t[at] of a writable texture: a place to load from, store to or use atomically.
  Ex texel(const Ex& e, const RV& idx) {
    const RV obj = value(e);
    const TexShape s = shape(obj.t.rk);
    if (!s.uav) fail("an element of a read-only texture");
    Ex out;
    out.is_lv = true;
    out.lv.kind = LV::Texel;
    out.lv.t = scalar(is_float(obj.t.elem) ? B::Float : obj.t.elem, obj.t.en);
    out.lv.handle = obj.c[0];
    out.lv.coords = parts(idx, B::Uint, s.texel, 3);
    return out;
  }
  RV texel_load(const LV& lv) {
    const Type o = overload_of(lv.t.b), i = m_.i32();
    if (lv.t.n > 1) m_.add_flags(1ull << 13);  // typed UAV loads of more than one component
    std::vector<Value> args{lv.handle, m_.undef(i)};
    append(args, lv.coords);
    append(args, {m_.undef(i), m_.undef(i), m_.undef(i)});
    const Value r = m_.dx_call(66, "textureLoad", o, res_ret(o), {m_.handle_type(), i, i, i, i, i, i, i}, Attr::ReadOnly, args);
    return from_ret(r, lv.t.b, lv.t.n);
  }
  void texel_store(const LV& lv, const RV& v) {
    // Four values and mask 15 always; a narrower view's missing values repeat the first (as DXC).
    const Type o = overload_of(lv.t.b), i = m_.i32();
    std::vector<Value> args{lv.handle};
    append(args, lv.coords);
    for (size_t k = 0; k < 4; ++k) args.push_back(k < v.c.size() ? v.c[k] : v.c[0]);
    args.push_back(m_.u8(15));
    m_.dx_call(67, "textureStore", o, m_.void_type(), {m_.handle_type(), i, i, i, o, o, o, o, m_.i8()}, Attr::None, args);
  }
  // InterlockedOp(t[at], value[, original]) and InterlockedCompareExchange(t[at], cmp, value, original).
  RV texel_atomic(const std::string& n, const std::vector<Ex>& a) {
    const LV& lv = a[0].lv;
    const Type i = m_.i32(), hd = m_.handle_type();
    Value old;
    size_t out_at;
    if (n == "InterlockedCompareExchange") {
      if (a.size() != 4) fail(n + " arguments");
      std::vector<Value> args{lv.handle};
      append(args, lv.coords);
      args.push_back(scalar_of(value(a[1]), lv.t.b));
      args.push_back(scalar_of(value(a[2]), lv.t.b));
      old = m_.dx_call(79, "atomicCompareExchange", i, i, {hd, i, i, i, i, i}, Attr::None, args);
      out_at = 3;
    } else {
      const bool sg = lv.t.b == B::Int;
      uint32_t op = 0;  // DXIL's atomic ops: add, and, or, xor, imin, imax, umin, umax, exchange
      if (n == "InterlockedAdd") op = 0;
      else if (n == "InterlockedAnd") op = 1;
      else if (n == "InterlockedOr") op = 2;
      else if (n == "InterlockedXor") op = 3;
      else if (n == "InterlockedMin") op = sg ? 4 : 6;
      else if (n == "InterlockedMax") op = sg ? 5 : 7;
      else if (n == "InterlockedExchange") op = 8;
      else fail(n);
      std::vector<Value> args{lv.handle, m_.u32(op)};
      append(args, lv.coords);
      args.push_back(scalar_of(value(a[1]), lv.t.b));
      old = m_.dx_call(78, "atomicBinOp", i, i, {hd, i, i, i, i, i}, Attr::None, args);
      out_at = 2;
    }
    if (a.size() > out_at) {
      if (!a[out_at].is_lv) fail(n + "'s original value into a value");
      store(a[out_at].lv, make(lv.t.b, {old}));
    }
    return RV{};
  }

  Module& m_;
  Syms& syms_;
  std::vector<Token> t_;
  size_t pos_ = 0;
};

}  // namespace

bool compile_hlsl(std::string_view source, std::string_view entry, Module& m, std::string& error) {
  try {
    Syms syms;
    std::vector<Token> raw;
    lex(source, syms, raw);
    Preprocessor pp(syms, raw);
    std::vector<Token> toks = pp.run();
    Compiler c(m, syms, std::move(toks));
    c.run(entry);
    return true;
  } catch (const Failure& f) {
    error = f.what;
    return false;
  }
}

}  // namespace am::shader::dxil
