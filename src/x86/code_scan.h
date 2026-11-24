#pragma once

// Finds the instructions of a PE image's executable sections and what each needs from the CPU.
//
// Two passes:
//  1. Recursive descent from known code (exception-table function starts, the entry point,
//     exports, TLS callbacks), following direct branches and fall-through. What it reaches is
//     code for certain: "confirmed".
//  2. A linear sweep over the bytes the first pass did not reach (switch cases reached through
//     jump tables, leaf functions nothing calls directly, data). Instructions found there are
//     "in function" when inside an exception-table range, else "outside": likely, not certain.
//
// The loader patches only confirmed sites; the others are handled when they execute.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "host/cpu.h"
#include "pe/pe_image.h"

namespace am::x86 {

enum class Confidence : uint8_t { Confirmed = 1, InFunction = 2, Outside = 3 };
constexpr size_t kConfidenceSlots = 4;  // index by Confidence; slot 0 unused
const char* confidence_name(Confidence c);

struct Site {
  uint32_t rva = 0;
  uint8_t length = 0;
  Confidence confidence = Confidence::Confirmed;
  host::CpuFeature feature = host::CpuFeature::SSE3;
};

struct CodeScan {
  using Counts = std::array<uint64_t, kConfidenceSlots>;
  Counts instructions{};
  Counts decode_errors{};
  Counts not_on_console{};   // encodings the console's CPU lacks: data decoded as code
  uint64_t overlaps = 0;     // a path reached the middle of an instruction already found
  uint64_t bytes = 0;        // executable bytes scanned
  uint64_t padding = 0;      // INT3 / zero padding skipped by the sweep
  uint64_t functions = 0;    // exception-table entries used as starting points
  std::array<Counts, host::kCpuFeatureCount> by_feature{};
  std::vector<Site> sites;   // every instruction needing one of the requested features
  std::vector<uint32_t> not_on_console_confirmed;  // where confirmed code has such an encoding (first 256)
};

CodeScan scan_code(const pe::Image& image, std::span<const uint8_t> file,
                   std::span<const host::CpuFeature> site_features);

}  // namespace am::x86
