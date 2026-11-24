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

}  // namespace am::rt::hle
