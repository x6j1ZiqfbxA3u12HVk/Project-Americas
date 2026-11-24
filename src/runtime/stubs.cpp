#include "runtime/stubs.h"

#include <windows.h>

#include <cstring>

#include "runtime/log.h"

namespace am::rt {
namespace {

constexpr size_t kThunkSize = 32;
constexpr size_t kPageSize = 64 * 1024;

UnimplementedMode g_mode = UnimplementedMode::Zero;

// Reached by a jump from a thunk, with the import's StubInfo in RCX and the caller's return
// address on the stack: returning from here returns to the title with RAX = 0.
extern "C" uint64_t am_stub_called(StubInfo* info) {
  const long n = InterlockedIncrement(&info->calls);
  if (n == 1) logf("unimplemented: %s!%s called (%s)", info->dll.c_str(), info->function.c_str(), info->why.c_str());
  if (g_mode == UnimplementedMode::Trap) {
    const ULONG_PTR args[1] = {reinterpret_cast<ULONG_PTR>(info)};
    RaiseException(kUnimplementedException, EXCEPTION_NONCONTINUABLE, 1, args);
  }
  return 0;
}

}  // namespace

void* StubFactory::stub(const std::string& dll, const std::string& function, const std::string& why) {
  const std::string key = dll + "!" + function;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [k, p] : by_key_) {
    if (k == key) return p;
  }
  g_mode = mode_;
  if (!page_ || used_ + kThunkSize > kPageSize) {
    // Executable and writable for its lifetime: stubs are also made while title threads run others
    // in the same page, so the protection must never flip.
    page_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    used_ = 0;
    if (!page_) return nullptr;
  }
  auto* info = new StubInfo{dll, function, why};
  infos_.push_back(info);

  // mov rcx, info ; mov rax, am_stub_called ; jmp rax
  uint8_t* t = page_ + used_;
  const uint64_t info_addr = reinterpret_cast<uint64_t>(info);
  const uint64_t handler = reinterpret_cast<uint64_t>(&am_stub_called);
  t[0] = 0x48;
  t[1] = 0xB9;
  std::memcpy(t + 2, &info_addr, 8);
  t[10] = 0x48;
  t[11] = 0xB8;
  std::memcpy(t + 12, &handler, 8);
  t[20] = 0xFF;
  t[21] = 0xE0;
  std::memset(t + 22, 0xCC, kThunkSize - 22);
  FlushInstructionCache(GetCurrentProcess(), t, kThunkSize);
  used_ += kThunkSize;
  by_key_.emplace_back(key, t);
  return t;
}

std::vector<const StubInfo*> StubFactory::all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {infos_.begin(), infos_.end()};
}

}  // namespace am::rt
