#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace am {

// Where Project Americas keeps everything it generates, and where the user's game files are.
struct Config {
  std::filesystem::path root;      // %AMERICAS_ROOT%, default C:\Project Americas
  std::filesystem::path ini_path;  // <root>\americas.ini
  bool ini_found = false;
  std::filesystem::path game_dir;  // the unpacked game; read-only; empty when not configured

  std::filesystem::path logs_dir() const { return root / L"logs"; }
  std::filesystem::path cache_dir() const { return root / L"cache"; }
  std::filesystem::path userdata_dir() const { return root / L"userdata"; }
};

// Reads the settings. Problems in americas.ini (unknown keys, malformed lines) go to `warnings`.
Config load_config(std::vector<std::string>& warnings);

}  // namespace am
