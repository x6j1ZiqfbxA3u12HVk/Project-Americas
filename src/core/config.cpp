#include "core/config.h"

#include <windows.h>

#include <fstream>
#include <iterator>

#include "core/text.h"

namespace am {

Config load_config(std::vector<std::string>& warnings) {
  Config config;
  wchar_t buf[1024];
  const DWORD n = GetEnvironmentVariableW(L"AMERICAS_ROOT", buf, static_cast<DWORD>(std::size(buf)));
  config.root = (n > 0 && n < std::size(buf)) ? std::filesystem::path(buf) : std::filesystem::path(L"C:\\Project Americas");
  config.ini_path = config.root / L"americas.ini";

  std::ifstream in(config.ini_path, std::ios::binary);
  if (!in) return config;
  config.ini_found = true;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // UTF-8 BOM

  const std::string where = narrow(config.ini_path.wstring());
  size_t pos = 0;
  int line_no = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string_view line = trim(std::string_view(text).substr(pos, end - pos));
    pos = end + 1;
    ++line_no;
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;

    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      warnings.push_back(where + ":" + std::to_string(line_no) + ": expected key = value");
      continue;
    }
    const std::string key = to_lower_ascii(trim(line.substr(0, eq)));
    std::string_view value = trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);

    if (key == "game_dir") {
      config.game_dir = std::filesystem::path(widen(value));
    } else if (key == "console") {
      const std::string v = to_lower_ascii(value);
      if (v == "auto" || v == "series_s" || v == "series_x") config.console = v;
      else warnings.push_back(where + ":" + std::to_string(line_no) + ": console must be auto, series_s or series_x");
    } else if (key == "shader_service") {
      const std::string v = to_lower_ascii(value);
      if (v == "on" || v == "auto") config.shader_service = true;
      else if (v == "off") config.shader_service = false;
      else warnings.push_back(where + ":" + std::to_string(line_no) + ": shader_service must be on or off");
    } else if (key == "shader_jobs") {
      const std::string v(value);
      if (!v.empty() && v.size() <= 3 && v.find_first_not_of("0123456789") == std::string::npos) {
        config.shader_jobs = static_cast<unsigned>(std::stoul(v));
      } else {
        warnings.push_back(where + ":" + std::to_string(line_no) + ": shader_jobs must be a number (0 = automatic)");
      }
    } else {
      warnings.push_back(where + ":" + std::to_string(line_no) + ": unknown key '" + key + "'");
    }
  }
  return config;
}

}  // namespace am
