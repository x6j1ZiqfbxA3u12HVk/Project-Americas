#pragma once

// Our implementations of the title's imports.

#include <string_view>
#include <vector>

namespace am::rt::hle {

struct Function {
  const char* module;  // console module ("xmem.dll"); nullptr: a wrapper for a host function, any module
  const char* name;
  void* address;
  bool passthrough = false;  // forwards to the host unchanged for now (not yet given console behaviour)
};

class Registry {
 public:
  Registry();
  // A console module's function (module compared case-insensitively).
  const Function* find(std::string_view module, std::string_view name) const;
  // A wrapper for a host function, by name.
  const Function* find_wrapper(std::string_view name) const;
  const std::vector<Function>& all() const { return functions_; }

 private:
  std::vector<Function> functions_;
};

// Each file of implementations adds its functions here.
void add_identity(std::vector<Function>& out);
void add_system(std::vector<Function>& out);
void add_network(std::vector<Function>& out);
void add_memory(std::vector<Function>& out);
void add_graphics(std::vector<Function>& out);
void add_audio(std::vector<Function>& out);          // AcpHal.dll
void add_malloc_trace(std::vector<Function>& out);   // AMERICAS_TRACE_MALLOC: the CRT heap, traced
void add_audio_streams(std::vector<Function>& out);  // xapu.dll, chad.dll

}  // namespace am::rt::hle

namespace am::rt::files {
struct Roots;
void configure(const Roots& roots);  // before the title runs
void add_files(std::vector<hle::Function>& out);
// For our own readers (DirectStorage): a read-only handle by the title's rules, or
// INVALID_HANDLE_VALUE with the last error set.
void* open_for_reading(const wchar_t* path);
}

namespace am {
class JsonWriter;
}
namespace am::rt::storage {
void add_storage(std::vector<hle::Function>& out);  // XCRDAPI.dll
void write_storage_report(JsonWriter& j);
}

namespace am::rt::input {
void add_input(std::vector<hle::Function>& out);  // GameInput.dll
}
