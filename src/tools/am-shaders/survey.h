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

namespace am::tools {

// One container's programs through the translator: HLSL written to `out_dir`, each compiled
// with DXC, failures named.
int translate(const std::string& wanted, const std::filesystem::path& pack_dir, const std::filesystem::path& out_dir);

// Every program through the translator: what translates, by stage; which instructions hold
// the rest up; DXC on a sample of `compile_sample` translations.
int image_survey(const std::filesystem::path& pack_dir);
// Compiles, with the runtime's binding, the programs the game's saved pipelines use (or every
// program with `all`) to <out>\<marker>.dxil, where the runtime looks them up. Resumable: what
// is there is skipped. With `refresh` programs already compiled are translated again, and only
// those whose source changed (a translator fix) are compiled again. Compiled DXIL is also kept
// by its source's content key (dxil\by-source): no source is ever compiled twice, under any
// marker or translator version. Files from before content keys are adopted as they are.
int precompile(const std::filesystem::path& pack_dir, const std::filesystem::path& runtime_dir,
               const std::filesystem::path& out_dir, bool all, const std::filesystem::path& hlsl_dir = {},
               bool refresh = false);
int translation_survey(const std::filesystem::path& pack_dir, const std::filesystem::path& out_json, size_t compile_sample,
                       const std::filesystem::path& hlsl_dir = {});

}  // namespace am::tools
