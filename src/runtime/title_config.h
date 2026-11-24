#pragma once

// What MicrosoftGame.Config says about the title: its executable, ids and storage.

#include <cstdint>
#include <filesystem>
#include <string>

namespace am::rt {

struct TitleConfig {
  std::filesystem::path executable;  // the first <Executable Name=...>, beside the config
  uint32_t title_id = 0;             // <TitleId>, hexadecimal
  std::string identity_name;         // <Identity Name=...>
  std::string identity_publisher;
  std::string version;               // <Identity Version=...>
  uint64_t local_storage_mb = 0;     // <PersistentLocalStorage><SizeMB>
};

// Reads <dir>\MicrosoftGame.Config; fields stay empty or 0 where it says nothing.
TitleConfig read_title_config(const std::filesystem::path& dir);

// The running title's, set before it starts (the GDK areas answer from it).
void set_title_config(const TitleConfig& config);
const TitleConfig& title_config();

}  // namespace am::rt
