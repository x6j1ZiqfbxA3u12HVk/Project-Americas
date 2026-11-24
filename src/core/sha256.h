#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace am {

using Sha256 = std::array<uint8_t, 32>;

// SHA-256 through the system's CNG provider. Used for build identities of title binaries.
std::optional<Sha256> sha256(std::span<const uint8_t> data);

}  // namespace am
