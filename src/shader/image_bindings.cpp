#include "shader/image_bindings.h"

#include <algorithm>
#include <array>

namespace am::shader {

// FNV-1a over the words, folded; the second slot mixes it further (the translated program
// computes the same).
uint32_t binding_hash(std::span<const uint32_t, 8> words) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t w : words) h = (h ^ w) * 0x01000193u;
  return h ^ (h >> 16);
}

uint32_t binding_hash2(uint32_t h) {
  h ^= 0x5BD1E995u;
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  return h ^ (h >> 16);
}

namespace {

constexpr uint32_t kHeader = 32, kImageEntry = 64, kSamplerEntry = 48, kMissRecord = 48;

// Cuckoo hashing: each key in one of its two slots. False when the table must grow.
template <typename Key>
bool place(const std::vector<Key>& keys, uint32_t capacity, std::vector<int>& slot_of) {
  std::vector<int> owner(capacity, -1);
  const auto slot = [&](int k, int which) {
    const uint32_t h = binding_hash(keys[k]);
    return static_cast<int>((which == 0 ? h : binding_hash2(h)) & (capacity - 1));
  };
  for (int k = 0; k < static_cast<int>(keys.size()); ++k) {
    const int a = slot(k, 0), b = slot(k, 1);
    if ((owner[a] >= 0 && keys[owner[a]] == keys[k]) || (owner[b] >= 0 && keys[owner[b]] == keys[k])) continue;  // twice
    int cur = k, at = a;
    bool placed = false;
    for (int kick = 0; kick < 128 && !placed; ++kick) {
      std::swap(cur, owner[at]);
      if (cur < 0) placed = true;
      else at = slot(cur, 0) == at ? slot(cur, 1) : slot(cur, 0);  // the displaced key's other slot
    }
    if (!placed) return false;
  }
  slot_of.assign(keys.size(), -1);
  for (uint32_t s = 0; s < capacity; ++s) {
    if (owner[s] >= 0) slot_of[owner[s]] = static_cast<int>(s);
  }
  // A repeated key points at its first copy's slot.
  for (int k = 0; k < static_cast<int>(keys.size()); ++k) {
    if (slot_of[k] >= 0) continue;
    for (int which = 0; which < 2; ++which) {
      const int s = slot(k, which);
      if (owner[s] >= 0 && keys[owner[s]] == keys[k]) slot_of[k] = s;
    }
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
