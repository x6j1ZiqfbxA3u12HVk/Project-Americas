#pragma once

// Redirects one function in the import tables of modules the Windows loader loaded.
//
// Used for the few system functions whose answer must cover title modules even when a host DLL
// asks: RtlPcToFileHeader, which the C++ runtime uses to find the image of a thrown object's
// type information. Our replacement answers for title images and forwards everything else, so
// the host modules see no difference.

#include <windows.h>

#include <cstddef>

namespace am::rt {

size_t patch_module_imports(HMODULE module, const void* original, const void* replacement);
size_t patch_loaded_imports(const void* original, const void* replacement);

}  // namespace am::rt
