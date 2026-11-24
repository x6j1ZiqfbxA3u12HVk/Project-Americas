#pragma once

// How translated programs find their images and samplers.
//
// A console program names a texture by its T# (8 dwords) and a sampler by its S# (4 dwords):
// values in SGPRs, known only when the program runs. The host keeps a table that maps those
// words to its own D3D12 descriptors: an open-addressed hash in one buffer (am_bindings),
// built here and probed by the translated program. What the table lacks the program queues
// (the miss list) for the host to add, and meanwhile reads as zero.
//
// The buffer, in bytes:
//   0   header: images at, images capacity, samplers at, samplers capacity,
//               misses at, misses capacity, misses written (atomic), 0
//   images:   capacity entries of 64 bytes: the T#'s 8 words, then srv, alt, uav, flags, 4 zeros
//   samplers: capacity entries of 48 bytes: the S#'s 4 words and 4 zeros, then index, 2 zeros, flags
//   misses:   capacity records of 48 bytes: kind (1 image, 2 sampler), 3 zeros, the 8 words
// Both kinds keep their values as a uint4 at byte 32, flags last. A slot is free while its
// flags are 0. Capacities are powers of two; a key sits within kProbes slots of its hash.

#include <cstdint>
#include <span>
#include <vector>

namespace am::shader {

// Image entry flags.
constexpr uint32_t kImageFloat = 0, kImageUint = 1, kImageSint = 2;  // bits 0-1: the view's numeric class
constexpr uint32_t kImageArray = 4;     // the srv / uav are array views (Texture2DArray, TextureCubeArray, ...)
constexpr uint32_t kImageHasUav = 8;    // uav is a writable view
constexpr uint32_t kImageLoadUav = 16;  // loads read the uav (the texture is written in the same pass)
constexpr uint32_t kBindingValid = 0x80000000u;
// Sampler entry flags.
constexpr uint32_t kSamplerCompare = 1;  // a comparison sampler (SamplerComparisonState)

constexpr uint32_t kProbes = 16;

// One image: its T#, and the heap indices of its views. `srv` has the T#'s own type
// (TextureCube for a cube); `alt` is a 2D-array view of a cube's faces, for loads.
struct ImageBinding {
  uint32_t words[8] = {};
  uint32_t srv = 0, alt = 0, uav = 0;
  uint32_t flags = 0;  // kBindingValid is added
};
struct SamplerBinding {
  uint32_t words[4] = {};
  uint32_t index = 0;
  uint32_t flags = 0;
};

uint32_t binding_hash(std::span<const uint32_t, 8> words);

// The am_bindings buffer, as dwords; empty if a key does not fit in kProbes slots of
// the table size chosen (it grows until all fit).
std::vector<uint32_t> build_bindings(std::span<const ImageBinding> images, std::span<const SamplerBinding> samplers,
                                     uint32_t miss_capacity = 64);

}  // namespace am::shader
