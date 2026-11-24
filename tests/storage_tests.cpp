// The console DirectStorage through its export, as the title calls it: a factory, a queue, status
// entries, requests from memory (zlib and plain) into memory.

#include <windows.h>

#include <cstring>
#include <vector>

#include "formats/inflate.h"
#include "runtime/gdk/runtime.h"
#include "runtime/hle/registry.h"
#include "test.h"

namespace {

struct Request {
  uint64_t options;
  void* destination;
  uint64_t page_offset;
  uint32_t destination_size;
  uint32_t reserved;
  const void* source;
  uint64_t offset;
  uint32_t source_size;
  uint32_t intermediate_size;
  uint64_t tag;
  const char* name;
};
struct QueueDesc {
  uint64_t source_type;
  uint16_t capacity;
  int8_t priority;
  const char* name;
  uint64_t unknown;
};
constexpr uint64_t kZlib = 1, kBcpack = 2, kMemorySource = 1ull << 22;

template <class F>
F slot(void* object, int n) {
  return reinterpret_cast<F>((*static_cast<void***>(object))[n]);
}

std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> z = {0x78, 0x01, 0x01};
  const uint16_t len = static_cast<uint16_t>(data.size()), nlen = static_cast<uint16_t>(~len);
  z.insert(z.end(), {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(nlen),
                     static_cast<uint8_t>(nlen >> 8)});
  z.insert(z.end(), data.begin(), data.end());
  const uint32_t a = am::formats::adler32(data);
  z.insert(z.end(), {static_cast<uint8_t>(a >> 24), static_cast<uint8_t>(a >> 16), static_cast<uint8_t>(a >> 8),
                     static_cast<uint8_t>(a)});
  return z;
}

bool wait_complete(void* status, uint32_t index) {
  using IsComplete = bool(WINAPI*)(void*, uint32_t);
  for (int i = 0; i < 5000; ++i) {
    if (slot<IsComplete>(status, 3)(status, index)) return true;
    Sleep(1);
  }
  return false;
}

}  // namespace

TEST_CASE(dstorage_queue_status_order) {
  const am::rt::hle::Registry registry;
  const auto* f = registry.find("xcrdapi.dll", "DStorageGetFactory");
  CHECK(f != nullptr);
  if (!f) return;
  using GetFactory = HRESULT(WINAPI*)(const GUID*, void**);
  const auto get_factory = reinterpret_cast<GetFactory>(f->address);
  const GUID iid = am::rt::gdk::guid("62959d11-d45f-4884-a0a3-f7862ea243ae");
  void* factory = nullptr;
  CHECK(get_factory(&iid, &factory) == S_OK && factory);
  const GUID other = am::rt::gdk::guid("00000000-0000-0000-0000-000000000002");
  void* none = nullptr;
  CHECK(get_factory(&other, &none) == E_NOINTERFACE && !none);

  using SetStaging = HRESULT(WINAPI*)(void*, uint32_t);
  using CreateStatus = HRESULT(WINAPI*)(void*, uint32_t, const char*, const GUID*, void**);
  using CreateQueue = HRESULT(WINAPI*)(void*, const QueueDesc*, const GUID*, void**);
  CHECK(slot<SetStaging>(factory, 8)(factory, 32u << 20) == S_OK);
  void* status = nullptr;
  CHECK(slot<CreateStatus>(factory, 5)(factory, 4, "test", &iid, &status) == S_OK && status);
  const QueueDesc desc{1, 8, 0, "test queue", 1};
  void* queue = nullptr;
  CHECK(slot<CreateQueue>(factory, 10)(factory, &desc, &iid, &queue) == S_OK && queue);
  if (!status || !queue) return;

  using Enqueue = void(WINAPI*)(void*, const Request*);
  using EnqueueStatus = void(WINAPI*)(void*, void*, uint32_t);
  using Submit = void(WINAPI*)(void*);
  using Result = HRESULT(WINAPI*)(void*, uint32_t);
  const auto enqueue = slot<Enqueue>(queue, 3);
  const auto enqueue_status = slot<EnqueueStatus>(queue, 4);
  const auto submit = slot<Submit>(queue, 6);
  const auto result = slot<Result>(status, 4);

  // A zlib request from memory, then a plain copy, then status 0.
  const std::vector<uint8_t> text = {'c', 'o', 'n', 's', 'o', 'l', 'e', ' ', 'd', 'a', 't', 'a'};
  const std::vector<uint8_t> packed = zlib_stored(text);
  std::vector<uint8_t> out(text.size(), 0), copy(4, 0);
  Request r{};
  r.options = kZlib | kMemorySource;
  r.source = packed.data();
  r.source_size = static_cast<uint32_t>(packed.size());
  r.destination = out.data();
  r.destination_size = static_cast<uint32_t>(out.size());
  enqueue(queue, &r);
  const uint32_t word = 0x11223344;
  Request plain{};
  plain.options = kMemorySource;
  plain.source = &word;
  plain.source_size = plain.destination_size = 4;
  plain.destination = copy.data();
  enqueue(queue, &plain);
  enqueue_status(queue, status, 0);
  submit(queue);
  CHECK(wait_complete(status, 0) && result(status, 0) == S_OK);
  CHECK(out == text);
  uint32_t got = 0;
  std::memcpy(&got, copy.data(), 4);
  CHECK(got == word);

  // A refused request fails the status after it, and only that one.
  Request bcpack = plain;
  bcpack.options |= kBcpack;
  enqueue(queue, &bcpack);
  enqueue_status(queue, status, 1);
  enqueue(queue, &plain);
  enqueue_status(queue, status, 2);
  submit(queue);
  CHECK(wait_complete(status, 1) && result(status, 1) == E_NOTIMPL);
  CHECK(wait_complete(status, 2) && result(status, 2) == S_OK);

  using Close = void(WINAPI*)(void*);
  using Release = ULONG(WINAPI*)(void*);
  slot<Close>(queue, 8)(queue);
  slot<Release>(queue, 2)(queue);
  slot<Release>(status, 2)(status);
}
