#pragma once

#include <filesystem>

namespace am::paths {

// Absolute, lexically normalised, without a trailing separator (a drive root keeps its own).
std::filesystem::path normalize(const std::filesystem::path& p);

// True when `candidate` is `root` or lies below it: component by component, ignoring case as
// NTFS does by default. The check is lexical; code holding an open handle must also check the
// handle's final path, which resolves links and junctions.
bool is_inside(const std::filesystem::path& candidate, const std::filesystem::path& root);

}  // namespace am::paths
