#include "runtime/files/console_paths.h"

#include "core/paths.h"

namespace am::rt::files {
namespace {

wchar_t upper(wchar_t c) { return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - 32) : c; }
bool is_separator(wchar_t c) { return c == L'\\' || c == L'/'; }

}  // namespace

const char* area_name(Area a) {
  switch (a) {
    case Area::Title: return "title";
    case Area::Persistent: return "persistent";
    case Area::Temp: return "temp";
    case Area::Denied: return "denied";
  }
  return "?";
}

PathMap::PathMap(Roots roots) : roots_(std::move(roots)) {
  roots_.title = paths::normalize(roots_.title);
  roots_.persistent = paths::normalize(roots_.persistent);
  roots_.temp = paths::normalize(roots_.temp);
  current_ = roots_.title;
}

const std::filesystem::path& PathMap::root(Area a) const {
  switch (a) {
    case Area::Persistent: return roots_.persistent;
    case Area::Temp: return roots_.temp;
    default: return roots_.title;
  }
}

bool PathMap::inside(const Resolved& r, const std::filesystem::path& host) const {
  return r.area != Area::Denied && paths::is_inside(host, root(r.area));
}

bool PathMap::set_current(const Resolved& r) {
  if (r.area == Area::Denied) return false;
  current_ = r.host;
  return true;
}

Resolved PathMap::resolve(std::wstring_view p) const {
  Resolved r;
  if (p.empty()) {
    r.why = "empty path";
    return r;
  }
  // \\?\C:\... is a host path written for long names; \\.\ is a device.
  if (p.starts_with(L"\\\\?\\")) p.remove_prefix(4);
  else if (p.size() >= 2 && is_separator(p[0]) && is_separator(p[1])) {
    r.why = "a device or network path";
    return r;
  }
  std::filesystem::path host;
  Area area = Area::Denied;
  if (p.size() >= 2 && p[1] == L':') {
    const wchar_t drive = upper(p[0]);
    std::wstring_view rest = p.substr(2);
    while (!rest.empty() && is_separator(rest.front())) rest.remove_prefix(1);
    if (drive == L'S') {
      area = Area::Persistent;
      host = roots_.persistent / std::filesystem::path(rest);
    } else if (drive == L'T') {
      area = Area::Temp;
      host = roots_.temp / std::filesystem::path(rest);
    } else if (drive == L'G') {
      area = Area::Title;
      host = roots_.title / std::filesystem::path(rest);
    } else {
      // A host path: allowed only inside one of the areas.
      host = std::filesystem::path(p);
      for (Area a : {Area::Title, Area::Persistent, Area::Temp}) {
        if (paths::is_inside(host, root(a))) {
          area = a;
          break;
        }
      }
      if (area == Area::Denied) {
        r.why = "outside the title's folder and userdata";
        r.host = paths::normalize(host);
        return r;
      }
    }
  } else if (is_separator(p[0])) {
    r.why = "a rooted path without a drive";
    return r;
  } else {
    host = current_ / std::filesystem::path(p);
    for (Area a : {Area::Title, Area::Persistent, Area::Temp}) {
      if (paths::is_inside(current_, root(a))) area = a;
    }
  }
  host = paths::normalize(host);
  // ".." must not climb out of the area.
  if (!paths::is_inside(host, root(area))) {
    r.why = "climbs out of its area";
    r.host = host;
    return r;
  }
  r.area = area;
  r.host = host;
  return r;
}

}  // namespace am::rt::files
