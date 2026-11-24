#pragma once

// Stand-ins for imports nothing provides yet. Each is a small generated thunk that records the
// call and then returns 0 to the caller, or raises an exception naming the import ("trap").

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace am::rt {

enum class UnimplementedMode : uint8_t { Zero, Trap };

struct StubInfo {
  std::string dll;
  std::string function;  // "#<ordinal>" when imported by ordinal
  std::string why;       // "console module not implemented", "wrapper not implemented", ...
  volatile long calls = 0;
};

// Exception code a trapping stub raises; the crash report names the import.
constexpr unsigned long kUnimplementedException = 0xE0414D01;

class StubFactory {
 public:
  explicit StubFactory(UnimplementedMode mode) : mode_(mode) {}
  StubFactory(const StubFactory&) = delete;
  StubFactory& operator=(const StubFactory&) = delete;
  // A callable address for this import; the same import gets the same stub.
  void* stub(const std::string& dll, const std::string& function, const std::string& why);
  // Every stub with its call count.
  std::vector<const StubInfo*> all() const;
  UnimplementedMode mode() const { return mode_; }

 private:
  UnimplementedMode mode_;
  mutable std::mutex mutex_;  // stubs are also made at run time (GetProcAddress)
  std::vector<StubInfo*> infos_;
  std::vector<std::pair<std::string, void*>> by_key_;
  uint8_t* page_ = nullptr;  // current thunk page
  size_t used_ = 0;
};

}  // namespace am::rt
