#include "x86/code_scan.h"

#include <algorithm>

#include "x86/decoder.h"

namespace am::x86 {
namespace {

// Per-byte state of an executable section.
enum : uint8_t {
  kLevelMask = 0x03,  // Confidence of the instruction covering the byte (0: none yet)
  kInterior = 0x04,   // not the first byte of its instruction
  kInFunction = 0x08, // inside an exception-table range
};

struct ExecSection {
  uint32_t rva = 0;
  std::span<const uint8_t> bytes;
  std::vector<uint8_t> marks;
};

class Scanner {
 public:
  Scanner(const pe::Image& image, std::span<const uint8_t> file, std::span<const host::CpuFeature> site_features)
      : image_(image), file_(file) {
    for (host::CpuFeature f : site_features) wanted_[static_cast<size_t>(f)] = true;
    for (const pe::Section& s : image.sections) {
      if (!(s.characteristics & pe::kSectionExecute)) continue;
      const uint32_t size = std::min(s.virtual_size ? s.virtual_size : s.raw_size, s.raw_size);
      const auto bytes = pe::bytes_at(image, file, s.rva, size);
      if (!bytes || bytes->empty()) continue;
      ExecSection e;
      e.rva = s.rva;
      e.bytes = *bytes;
      e.marks.assign(bytes->size(), 0);
      result_.bytes += bytes->size();
      sections_.push_back(std::move(e));
    }
  }

  CodeScan run() {
    // Starting points.
    const std::vector<pe::RuntimeFunction> functions = pe::read_function_table(image_, file_);
    for (const pe::RuntimeFunction& f : functions) {
      for (ExecSection& s : sections_) {
        const uint64_t begin = std::max<uint64_t>(f.begin, s.rva);
        const uint64_t end = std::min<uint64_t>(f.end, uint64_t{s.rva} + s.marks.size());
        for (uint64_t r = begin; r < end; ++r) s.marks[static_cast<size_t>(r - s.rva)] |= kInFunction;
      }
      if (find(f.begin)) {
        work_.push_back(f.begin);
        ++result_.functions;
      }
    }
    if (image_.entry_rva) work_.push_back(image_.entry_rva);
    for (const pe::Export& e : image_.exports) {
      if (e.forwarder.empty()) work_.push_back(e.rva);
    }
    if (image_.tls) {
      for (uint64_t va : image_.tls->callbacks) {
        if (va > image_.image_base) work_.push_back(static_cast<uint32_t>(va - image_.image_base));
      }
    }
    descend();
    sweep();
    return std::move(result_);
  }

 private:
  ExecSection* find(uint32_t rva) {
    for (ExecSection& s : sections_) {
      if (rva >= s.rva && rva - s.rva < s.marks.size()) return &s;
    }
    return nullptr;
  }

  void record(const Instruction& insn, uint32_t rva, Confidence level) {
    const auto slot = static_cast<size_t>(level);
    ++result_.instructions[slot];
    const Requirement req = requirement(insn);
    if (req.not_on_console) {
      ++result_.not_on_console[slot];
      if (level == Confidence::Confirmed && result_.not_on_console_confirmed.size() < 256) {
        result_.not_on_console_confirmed.push_back(rva);
      }
    }
    if (req.feature) {
      const auto f = static_cast<size_t>(*req.feature);
      ++result_.by_feature[f][slot];
      if (wanted_[f]) result_.sites.push_back({rva, insn.length, level, *req.feature});
    }
  }

  // Claims [index, index + length) for an instruction; fails if any byte is already taken.
  static bool claim(ExecSection& s, size_t index, uint8_t length, Confidence level) {
    if (index + length > s.marks.size()) return false;
    for (size_t k = 0; k < length; ++k) {
      if (s.marks[index + k] & kLevelMask) return false;
    }
    const auto lv = static_cast<uint8_t>(level);
    s.marks[index] |= lv;
    for (size_t k = 1; k < length; ++k) s.marks[index + k] |= static_cast<uint8_t>(lv | kInterior);
    return true;
  }

  void descend() {
    const auto slot = static_cast<size_t>(Confidence::Confirmed);
    while (!work_.empty()) {
      const uint32_t rva = work_.back();
      work_.pop_back();
      ExecSection* s = find(rva);
      if (!s) continue;
      const size_t index = rva - s->rva;
      const uint8_t mark = s->marks[index];
      if (mark & kLevelMask) {
        if (mark & kInterior) ++result_.overlaps;
        continue;
      }
      Instruction insn;
      if (decode(s->bytes.subspan(index), insn) != DecodeStatus::Ok) {
        ++result_.decode_errors[slot];
        continue;
      }
      if (!claim(*s, index, insn.length, Confidence::Confirmed)) {
        ++result_.overlaps;
        continue;
      }
      record(insn, rva, Confidence::Confirmed);
      const uint32_t next = rva + insn.length;
      if (insn.direct_branch()) work_.push_back(static_cast<uint32_t>(static_cast<int64_t>(next) + insn.branch_disp));
      if (insn.flow != Flow::Jump && insn.flow != Flow::IndirectJump && insn.flow != Flow::Return &&
          insn.flow != Flow::Stop) {
        work_.push_back(next);
      }
    }
  }

  void sweep() {
    for (ExecSection& s : sections_) {
      size_t i = 0;
      while (i < s.marks.size()) {
        if (s.marks[i] & kLevelMask) {
          ++i;
          continue;
        }
        const uint8_t b = s.bytes[i];
        if (b == 0xCC || b == 0x00) {
          ++result_.padding;
          ++i;
          continue;
        }
        const Confidence level = (s.marks[i] & kInFunction) ? Confidence::InFunction : Confidence::Outside;
        Instruction insn;
        if (decode(s.bytes.subspan(i), insn) != DecodeStatus::Ok) {
          ++result_.decode_errors[static_cast<size_t>(level)];
          ++i;
          continue;
        }
        if (!claim(s, i, insn.length, level)) {
          ++result_.overlaps;
          ++i;
          continue;
        }
        record(insn, s.rva + static_cast<uint32_t>(i), level);
        i += insn.length;
      }
    }
  }

  const pe::Image& image_;
  std::span<const uint8_t> file_;
  std::array<bool, host::kCpuFeatureCount> wanted_{};
  std::vector<ExecSection> sections_;
  std::vector<uint32_t> work_;
  CodeScan result_;
};

}  // namespace

const char* confidence_name(Confidence c) {
  switch (c) {
    case Confidence::Confirmed: return "confirmed";
    case Confidence::InFunction: return "in-function";
    case Confidence::Outside: return "outside";
  }
  return "?";
}

CodeScan scan_code(const pe::Image& image, std::span<const uint8_t> file,
                   std::span<const host::CpuFeature> site_features) {
  Scanner scanner(image, file, site_features);
  return scanner.run();
}

}  // namespace am::x86
