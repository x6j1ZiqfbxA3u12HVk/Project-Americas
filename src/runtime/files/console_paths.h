#pragma once

// Console paths onto host folders (plan, section 5.3), deny by default:
//
// * the title's folder (the game dump): read-only. The title reaches it by absolute paths (our
//   GetModuleFileName gives it the dump's own path), by relative paths (from its current
//   directory, the title's folder at start) and as G:\;
// * S:\ (persistent local storage) and T:\ (temp): read-write, under userdata;
// * everything else: refused, and so is any write into the title's folder.
//
// The check here is on the path; code that opens a handle checks the handle's final path too,
// which resolves links and junctions.

#include <filesystem>
#include <string>
#include <string_view>

namespace am::rt::files {

enum class Area : uint8_t { Title, Persistent, Temp, Denied };
const char* area_name(Area a);

struct Resolved {
  Area area = Area::Denied;
  std::filesystem::path host;  // normalized
  std::string why;             // for a refusal
  bool writable() const { return area == Area::Persistent || area == Area::Temp; }
};

struct Roots {
  std::filesystem::path title;
  std::filesystem::path persistent;
  std::filesystem::path temp;
};

class PathMap {
 public:
  explicit PathMap(Roots roots);
  Resolved resolve(std::wstring_view console_path) const;
  // Whether `host` (a handle's final path) lies inside the area `r` resolved to.
  bool inside(const Resolved& r, const std::filesystem::path& host) const;
  const std::filesystem::path& root(Area a) const;
  // _chdir: relative paths resolve from here (a host path inside an area).
  bool set_current(const Resolved& r);
  const Roots& roots() const { return roots_; }

 private:
  Roots roots_;
  std::filesystem::path current_;
};

}  // namespace am::rt::files
