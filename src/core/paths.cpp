#include "core/paths.h"

#include <windows.h>

#include <system_error>

namespace am::paths {

std::filesystem::path normalize(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(p, ec);
  if (ec) abs = p;
  abs = abs.lexically_normal();
  // "C:\dir\" normalises to a path with an empty last element; drop it, but keep "C:\".
  if (!abs.has_filename() && abs.has_relative_path()) abs = abs.parent_path();
  return abs;
}

static bool same_component(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
         CSTR_EQUAL;
}

bool is_inside(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  const std::filesystem::path c = normalize(candidate);
  const std::filesystem::path r = normalize(root);
  auto ci = c.begin();
  for (auto ri = r.begin(); ri != r.end(); ++ri, ++ci) {
    if (ci == c.end()) return false;
    if (!same_component(ri->native(), ci->native())) return false;
  }
  return true;
}

}  // namespace am::paths
