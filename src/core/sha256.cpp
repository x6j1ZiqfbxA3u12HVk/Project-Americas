#include "core/sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>

namespace am {

std::optional<Sha256> sha256(std::span<const uint8_t> data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return std::nullopt;

  std::optional<Sha256> result;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
    bool ok = true;
    size_t offset = 0;
    while (ok && offset < data.size()) {
      // BCryptHashData takes a 32-bit length.
      const ULONG chunk = static_cast<ULONG>(std::min<size_t>(data.size() - offset, size_t{1} << 30));
      ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data.data() + offset), chunk, 0));
      offset += chunk;
    }
    Sha256 digest{};
    if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
      result = digest;
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return result;
}

}  // namespace am
