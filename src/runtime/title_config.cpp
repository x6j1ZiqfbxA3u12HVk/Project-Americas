#include "runtime/title_config.h"

#include <fstream>
#include <iterator>

#include "core/text.h"

namespace am::rt {
namespace {

TitleConfig g_config;

// The text of the first <tag>...</tag>, trimmed.
std::string element(const std::string& text, const std::string& tag, size_t from = 0) {
  const size_t open = text.find("<" + tag + ">", from);
  if (open == std::string::npos) return {};
  const size_t start = open + tag.size() + 2;
  const size_t close = text.find("</" + tag + ">", start);
  if (close == std::string::npos) return {};
  return std::string(trim(std::string_view(text).substr(start, close - start)));
}

// The value of ` name="..."` inside the first <tag ...> element. The leading space keeps
// `Name=` from matching inside other attributes (OverrideDisplayName).
std::string attribute(const std::string& text, const std::string& tag, const std::string& name) {
  const size_t open = text.find("<" + tag + " ");
  if (open == std::string::npos) return {};
  const size_t end = text.find('>', open);
  const size_t at = text.find(" " + name + "=\"", open);
  if (at == std::string::npos || at > end) return {};
  const size_t value = at + name.size() + 3;
  const size_t quote = text.find('"', value);
  if (quote == std::string::npos || quote > end) return {};
  return text.substr(value, quote - value);
}

}  // namespace

TitleConfig read_title_config(const std::filesystem::path& dir) {
  TitleConfig c;
  std::ifstream in(dir / L"MicrosoftGame.Config", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.empty()) return c;
  const std::string exe = attribute(text, "Executable", "Name");
  if (!exe.empty()) c.executable = dir / widen(exe);
  c.title_id = static_cast<uint32_t>(std::strtoul(element(text, "TitleId").c_str(), nullptr, 16));
  c.identity_name = attribute(text, "Identity", "Name");
  c.identity_publisher = attribute(text, "Identity", "Publisher");
  c.version = attribute(text, "Identity", "Version");
  const size_t storage = text.find("<PersistentLocalStorage>");
  if (storage != std::string::npos) c.local_storage_mb = std::strtoull(element(text, "SizeMB", storage).c_str(), nullptr, 10);
  return c;
}

void set_title_config(const TitleConfig& config) { g_config = config; }
const TitleConfig& title_config() { return g_config; }

}  // namespace am::rt
