#pragma once

// Where each import of the title is bound. The loader binds by these rules and the census
// tool (am-inspect) reports them, so both always agree.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace am::loader {

enum class Route : uint8_t {
  Ours,        // a console-only module: our implementation
  Wrap,        // a host function the title needs with console behaviour: our wrapper around it
  Host,        // the host's own function, unchanged
  Title,       // a module shipped beside the title's executable, mapped like the executable
  Unresolved,  // nothing provides it: bound to a stub that reports the call
};
const char* route_name(Route r);

class ImportPolicy {
 public:
  // `title_modules`: file names of the DLLs beside the title's executable.
  explicit ImportPolicy(std::vector<std::string> title_modules = {});

  // `function` is the import's name. An import by ordinal is resolved to its name first where
  // the providing module is known; pass an empty name otherwise.
  Route route(std::string_view dll, std::string_view function) const;
  bool is_title_module(std::string_view dll) const;

 private:
  std::vector<std::string> title_modules_;  // lower case
};

// Console system libraries a PC does not have.
bool is_console_only_module(std::string_view dll);
// API sets and the Windows DLLs whose functions the title may call directly.
bool is_host_module(std::string_view dll);
// Why a host function is wrapped ("console paths", "title memory", ...); empty when it is not.
std::string_view wrap_reason(std::string_view function);

}  // namespace am::loader
