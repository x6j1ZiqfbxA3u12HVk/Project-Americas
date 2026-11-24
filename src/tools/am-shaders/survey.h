#pragma once

#include <filesystem>
#include <string>

namespace am::tools {

// Every console program of the corpus through the RDNA 2 decoder; counts, failures by reason,
// instruction histogram, written as JSON.
int survey(const std::filesystem::path& pack_dir, const std::filesystem::path& out_json);

// A listing of one container's programs (`wanted`: a hash prefix). With `raw_dir`, each
// program's code is also written there as <hash>_<n>.bin.
int disasm(const std::string& wanted, const std::filesystem::path& pack_dir, const std::filesystem::path& raw_dir);

}  // namespace am::tools
