#include "shader/image_bindings.h"

#include <algorithm>
#include <array>

namespace am::shader {

// FNV-1a over the words, folded (the translated program computes the same).
uint32_t binding_hash(std::span<const uint32_t, 8> words) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t w : words) h = (h ^ w) * 0x01000193u;
  return h ^ (h >> 16);
}

namespace {

constexpr uint32_t kHeader = 32, kImageEntry = 64, kSamplerEntry = 48, kMissRecord = 48;

// Places each key; false if one needs more than kProbes probes.
template <typename Key>
bool place(const std::vector<Key>& keys, uint32_t capacity, std::vector<int>& slot_of) {
  std::vector<int> owner(capacity, -1);
  slot_of.assign(keys.size(), -1);
  for (size_t k = 0; k < keys.size(); ++k) {
    const uint32_t h = binding_hash(keys[k]);
    bool placed = false;
    for (uint32_t p = 0; p < kProbes && !placed; ++p) {
      const uint32_t s = (h + p) & (capacity - 1);
      if (owner[s] < 0) {
        owner[s] = static_cast<int>(k);
        slot_of[k] = static_cast<int>(s);
        placed = true;
      } else if (keys[owner[s]] == keys[k]) {
        slot_of[k] = static_cast<int>(s);  // the same descriptor twice: the first entry stands
        placed = true;
      }
    }
    if (!placed) return false;
  }
  return true;
}

uint32_t capacity_for(size_t n) {
  uint32_t c = 8;
  while (c < n * 2) c <<= 1;
  return c;
}

}  // namespace

std::vector<uint32_t> build_bindings(std::span<const ImageBinding> images, std::span<const SamplerBinding> samplers,
                                     uint32_t miss_capacity) {
  using Key = std::array<uint32_t, 8>;
  std::vector<Key> ikeys, skeys;
  for (const ImageBinding& b : images) {
    Key k{};
    std::copy(std::begin(b.words), std::end(b.words), k.begin());
    ikeys.push_back(k);
  }
  for (const SamplerBinding& b : samplers) {
    Key k{};
    std::copy(std::begin(b.words), std::end(b.words), k.begin());
    skeys.push_back(k);
  }
  uint32_t icap = capacity_for(ikeys.size()), scap = capacity_for(skeys.size());
  std::vector<int> islot, sslot;
  while (!place(ikeys, icap, islot)) {
    if ((icap <<= 1) > (1u << 20)) return {};
  }
  while (!place(skeys, scap, sslot)) {
    if ((scap <<= 1) > (1u << 20)) return {};
  }
  const uint32_t images_at = kHeader, samplers_at = images_at + icap * kImageEntry;
  const uint32_t misses_at = samplers_at + scap * kSamplerEntry, bytes = misses_at + miss_capacity * kMissRecord;
  std::vector<uint32_t> out(bytes / 4, 0);
  const uint32_t header[8] = {images_at, icap, samplers_at, scap, misses_at, miss_capacity, 0, 0};
  std::copy(std::begin(header), std::end(header), out.begin());
  for (size_t k = 0; k < images.size(); ++k) {
    uint32_t* e = &out[(images_at + islot[k] * kImageEntry) / 4];
    if (e[11] != 0) continue;
    std::copy(std::begin(images[k].words), std::end(images[k].words), e);
    e[8] = images[k].srv;
    e[9] = images[k].alt;
    e[10] = images[k].uav;
    e[11] = images[k].flags | kBindingValid;
  }
  for (size_t k = 0; k < samplers.size(); ++k) {
    uint32_t* e = &out[(samplers_at + sslot[k] * kSamplerEntry) / 4];
    if (e[11] != 0) continue;
    std::copy(std::begin(samplers[k].words), std::end(samplers[k].words), e);
    e[8] = samplers[k].index;
    e[11] = samplers[k].flags | kBindingValid;
  }
  return out;
}

}  // namespace am::shader
