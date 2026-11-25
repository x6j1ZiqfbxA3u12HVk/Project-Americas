#include "runtime/hle/registry.h"

#include "core/text.h"
#include "runtime/gdk/runtime.h"

namespace am::rt::hle {

Registry::Registry() {
  add_identity(functions_);
  add_system(functions_);
  add_network(functions_);
  add_memory(functions_);
  add_graphics(functions_);
  add_audio(functions_);
  add_audio_streams(functions_);
  gdk::add_runtime_exports(functions_);
  input::add_input(functions_);
  files::add_files(functions_);
  storage::add_storage(functions_);
}

const Function* Registry::find(std::string_view module, std::string_view name) const {
  for (const Function& f : functions_) {
    if (f.module && iequals_ascii(f.module, module) && name == f.name) return &f;
  }
  return nullptr;
}

const Function* Registry::find_wrapper(std::string_view name) const {
  for (const Function& f : functions_) {
    if (!f.module && name == f.name) return &f;
  }
  return nullptr;
}

}  // namespace am::rt::hle
