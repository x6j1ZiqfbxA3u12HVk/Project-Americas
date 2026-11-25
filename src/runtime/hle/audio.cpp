// The console's audio hardware library, AcpHal.dll:
//
// * the APU heap: memory the CPU and the audio hardware share, each block with a 32-bit device
//   address (the hardware's view) beside its CPU address;
// * the SHAPE context arrays, allocated from that heap: six arrays (rate conversion,
//   EQ/compression, filter/volume, DMA, XMA, PCM), one context per voice;
// * the audio coprocessor object: a command queue and a message queue. Commands complete at
//   once and are acknowledged as the hardware would (a "command completed" message, the
//   notification word set).
//
// Not done yet: the XMA decoder and the other SHAPE units, so voices stay silent.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::hle {
namespace {

constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT kFail = static_cast<HRESULT>(0x80004005);
constexpr HRESULT kHeapAlreadyCreated = static_cast<HRESULT>(0x8ACA0001);

// ---- the APU heap ----

class ApuHeap {
 public:
  bool create() {
    std::lock_guard g(lock_);
    if (created_) return false;
    start();
    return true;
  }

  // Zeroed memory and its device address; nullptr when there is no room.
  void* allocate(uint32_t size, uint32_t align, uint32_t* device) {
    std::lock_guard g(lock_);
    if (!created_) start();
    align = std::max<uint32_t>(align, 16);
    const uint32_t padded = (std::max<uint32_t>(size, 1) + 15) & ~15u;
    uint32_t at = 0;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      const uint64_t aligned = (uint64_t{it->first} + align - 1) / align * align;
      if (aligned + padded > it->second) continue;
      at = static_cast<uint32_t>(aligned);
      const uint32_t begin = it->first, end = it->second;
      free_.erase(it);
      if (begin < at) free_[begin] = at;
      if (at + padded < end) free_[at + padded] = end;
      break;
    }
    if (!at) return nullptr;
    void* p = _aligned_malloc(padded, align);
    if (!p) {
      give_back(at, at + padded);
      return nullptr;
    }
    std::memset(p, 0, padded);
    const uintptr_t host = reinterpret_cast<uintptr_t>(p);
    blocks_[host] = {size, padded, at};
    by_device_[at] = host;
    if (device) *device = at;
    return p;
  }

  void release(void* p) {
    std::lock_guard g(lock_);
    auto it = blocks_.find(reinterpret_cast<uintptr_t>(p));
    if (it == blocks_.end()) return;
    const Block b = it->second;
    blocks_.erase(it);
    by_device_.erase(b.device);
    give_back(b.device, b.device + b.padded);
    _aligned_free(p);
  }

  // The device address of any byte of a block; 0 elsewhere.
  uint32_t device_of(const void* p) {
    std::lock_guard g(lock_);
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    auto it = blocks_.upper_bound(a);
    if (it == blocks_.begin()) return 0;
    --it;
    if (a >= it->first + std::max<uint32_t>(it->second.size, 1)) return 0;
    return it->second.device + static_cast<uint32_t>(a - it->first);
  }

  // The CPU address of a device address inside a block; nullptr elsewhere.
  void* host_of(uint32_t device) {
    std::lock_guard g(lock_);
    auto it = by_device_.upper_bound(device);
    if (it == by_device_.begin()) return nullptr;
    --it;
    const Block& b = blocks_[it->second];
    if (device >= it->first + std::max<uint32_t>(b.size, 1)) return nullptr;
    return reinterpret_cast<void*>(it->second + (device - it->first));
  }

 private:
  struct Block {
    uint32_t size = 0, padded = 0, device = 0;
  };

  // Device addresses start above 0, which means "none" to titles.
  void start() {
    created_ = true;
    free_[0x10000] = 0xFFFFFFF0u;
  }

  void give_back(uint32_t begin, uint32_t end) {
    auto after = free_.find(end);
    if (after != free_.end()) {
      end = after->second;
      free_.erase(after);
    }
    auto before = free_.lower_bound(begin);
    if (before != free_.begin()) {
      --before;
      if (before->second == begin) {
        begin = before->first;
        free_.erase(before);
      }
    }
    free_[begin] = end;
  }

  std::mutex lock_;
  bool created_ = false;
  std::map<uintptr_t, Block> blocks_;     // by CPU address
  std::map<uint32_t, uintptr_t> by_device_;
  std::map<uint32_t, uint32_t> free_;     // free device ranges, begin -> end
};

ApuHeap& heap() {
  static ApuHeap* const h = new ApuHeap;
  return *h;
}

// ApuCreateHeap(cached bytes, non-cached bytes): sizes the heap once, before anything is
// allocated. We allocate per block, so the sizes only decide whether the call is valid.
HRESULT WINAPI my_ApuCreateHeap(UINT32 cached, UINT32 non_cached) {
  logf("audio: ApuCreateHeap(%u KiB cached, %u KiB non-cached)", cached >> 10, non_cached >> 10);
  if (!cached) return kInvalidArg;
  return heap().create() ? S_OK : kHeapAlreadyCreated;
}

// ApuAlloc(&cpu address, &device address, size, alignment, flags).
HRESULT WINAPI my_ApuAlloc(void** out, UINT32* device, UINT32 size, UINT32 align, UINT32 /*flags*/) {
  if (!out || !size || align < 4 || (align & (align - 1))) return kInvalidArg;
  uint32_t d = 0;
  void* p = heap().allocate(size, align, &d);
  if (!p) return kOutOfMemory;
  *out = p;
  if (device) *device = d;
  return S_OK;
}

void WINAPI my_ApuFree(void* p) { heap().release(p); }
void* WINAPI my_ApuMapApuAddress(UINT32 device) { return heap().host_of(device); }
UINT32 WINAPI my_ApuMapVirtualAddress(const void* p) { return heap().device_of(p); }
BOOLEAN WINAPI my_ApuIsVirtualAddressValid(const void* p, UINT32 align) {
  const uint32_t d = heap().device_of(p);
  return d != 0 && (align == 0 || d % align == 0);
}

// ---- SHAPE contexts ----

#pragma pack(push, 1)
struct ShapeContexts {
  uint32_t counts[6];  // rate conversion, EQ/compression, filter/volume, DMA, XMA, PCM
  void* arrays[6];
  uint32_t device[6];
};
#pragma pack(pop)
static_assert(sizeof(ShapeContexts) == 0x60);

// Bytes per context: an XMA context is 16 words; rate conversion and DMA contexts as titles clear
// them; the others a generous bound.
constexpr uint32_t kContextBytes[6] = {32, 256, 256, 16, 64, 256};

std::mutex g_contexts_lock;
bool g_contexts = false;
void* g_context_arrays[6] = {};

HRESULT WINAPI my_AcpHalAllocateShapeContexts(ShapeContexts* request) {
  if (!request) return kInvalidArg;
  std::lock_guard g(g_contexts_lock);
  if (g_contexts) return kFail;
  ShapeContexts r;
  std::memcpy(&r, request, sizeof r);
  void* arrays[6] = {};
  uint32_t device[6] = {};
  for (int i = 0; i < 6; ++i) {
    if (!r.counts[i]) continue;
    arrays[i] = heap().allocate(r.counts[i] * kContextBytes[i], 256, &device[i]);
    if (!arrays[i]) {
      for (void* p : arrays) heap().release(p);
      return kOutOfMemory;
    }
  }
  std::memcpy(request->arrays, arrays, sizeof arrays);
  std::memcpy(request->device, device, sizeof device);
  std::memcpy(g_context_arrays, arrays, sizeof arrays);
  g_contexts = true;
  logf("audio: SHAPE contexts: %u rate, %u EQ, %u filter, %u DMA, %u XMA, %u PCM", r.counts[0], r.counts[1], r.counts[2],
       r.counts[3], r.counts[4], r.counts[5]);
  return S_OK;
}

HRESULT WINAPI my_AcpHalReleaseShapeContexts() {
  std::lock_guard g(g_contexts_lock);
  if (!g_contexts) return kFail;
  for (void*& p : g_context_arrays) {
    heap().release(p);
    p = nullptr;
  }
  g_contexts = false;
  return S_OK;
}

// ---- the audio coprocessor ----

constexpr uint32_t kLoadFlowgraph = 1, kRegisterMessages = 2, kUnregisterMessages = 3;
constexpr uint32_t kFlowgraphCompleted = 0x02, kCommandCompleted = 0x10, kDisconnected = 0x80;
constexpr uint64_t kAudioFramesPerSecond = 375;  // 128 samples at 48 kHz

#pragma pack(push, 1)
struct Message {
  uint32_t kind;
  uint32_t dropped;  // messages lost to a full queue before this one
  uint32_t usec;
  uint8_t data[16];
};
#pragma pack(pop)

struct Coprocessor;
struct CoprocessorMethods {
  HRESULT(WINAPI* connect)(Coprocessor*, UINT32 commands, UINT32 messages);
  HRESULT(WINAPI* disconnect)(Coprocessor*);
  HRESULT(WINAPI* submit)(Coprocessor*, UINT32 command, UINT64 id, UINT32 frame, const void* data, UINT32 notification);
  bool(WINAPI* pop_message)(Coprocessor*, Message*);
  UINT32(WINAPI* message_count)(Coprocessor*);
  void(WINAPI* release)(Coprocessor*);
};

struct Coprocessor {
  const CoprocessorMethods* methods;
  std::mutex lock;
  bool connected = false;
  uint32_t mask = 0;  // the message kinds the title registered for
  size_t capacity = 0;
  uint32_t dropped = 0;
  std::deque<Message> messages;
  LARGE_INTEGER since{}, frequency{};

  uint64_t usec() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart - since.QuadPart) * 1000000 / frequency.QuadPart);
  }
  void post(uint32_t kind, const void* data, size_t size) {
    if (!(mask & kind)) return;
    if (messages.size() >= capacity) {
      ++dropped;
      return;
    }
    Message m{};
    m.kind = kind;
    m.dropped = dropped;
    dropped = 0;
    m.usec = static_cast<uint32_t>(usec());
    std::memcpy(m.data, data, std::min(size, sizeof m.data));
    messages.push_back(m);
  }
};

// The command's first words; zeros when the title's pointer is bad.
void read_words(const void* data, uint32_t (&word)[2]) {
  __try {
    std::memcpy(word, data, sizeof word);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    word[0] = word[1] = 0;
  }
}

HRESULT WINAPI acp_connect(Coprocessor* self, UINT32 commands, UINT32 messages) {
  if (!commands) return kInvalidArg;
  std::lock_guard g(self->lock);
  self->connected = true;
  self->capacity = messages;
  logf("audio: coprocessor connected (%u commands, %u messages)", commands, messages);
  return S_OK;
}

HRESULT WINAPI acp_disconnect(Coprocessor* self) {
  std::lock_guard g(self->lock);
  if (!self->connected) return kFail;
  self->post(kDisconnected, nullptr, 0);
  self->connected = false;
  return S_OK;
}

// Done at once: registrations take effect, a flowgraph completes, a command with an id is
// acknowledged, and its notification word (a device address) is set.
HRESULT WINAPI acp_submit(Coprocessor* self, UINT32 command, UINT64 id, UINT32 /*frame*/, const void* data, UINT32 notification) {
  static std::atomic<uint64_t> seen{0};
  if (command < 64 && !(seen.fetch_or(uint64_t{1} << command) & (uint64_t{1} << command))) {
    logf("audio: first coprocessor command %u", command);
  }
  uint32_t word[2] = {};
  if (data) read_words(data, word);
  std::lock_guard g(self->lock);
  if (command == kRegisterMessages) self->mask |= word[0];
  else if (command == kUnregisterMessages) self->mask &= ~word[0];
  else if (command == kLoadFlowgraph) self->post(kFlowgraphCompleted, &word[1], 4);
  if (id) {
    uint8_t done[16];
    const uint32_t frame = static_cast<uint32_t>(self->usec() * kAudioFramesPerSecond / 1000000);
    std::memcpy(done, &command, 4);
    std::memcpy(done + 4, &id, 8);
    std::memcpy(done + 12, &frame, 4);
    self->post(kCommandCompleted, done, sizeof done);
  }
  if (notification) {
    if (void* w = heap().host_of(notification)) *static_cast<volatile uint32_t*>(w) = 1;
  }
  return S_OK;
}

bool WINAPI acp_pop_message(Coprocessor* self, Message* out) {
  std::lock_guard g(self->lock);
  if (self->messages.empty()) return false;
  if (out) std::memcpy(out, &self->messages.front(), sizeof(Message));
  self->messages.pop_front();
  return true;
}

UINT32 WINAPI acp_message_count(Coprocessor* self) {
  std::lock_guard g(self->lock);
  return static_cast<UINT32>(self->messages.size());
}

void WINAPI acp_release(Coprocessor* self) { delete self; }

const CoprocessorMethods kCoprocessorMethods = {acp_connect, acp_disconnect, acp_submit, acp_pop_message, acp_message_count,
                                                acp_release};

HRESULT WINAPI my_AcpHalCreate(Coprocessor** out) {
  if (!out) return kInvalidArg;
  Coprocessor* c = new Coprocessor;
  c->methods = &kCoprocessorMethods;
  QueryPerformanceFrequency(&c->frequency);
  QueryPerformanceCounter(&c->since);
  *out = c;
  return S_OK;
}

#define AM_ACPHAL(name) Function{"acphal.dll", #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void add_audio(std::vector<Function>& out) {
  out.insert(out.end(), {
                            AM_ACPHAL(ApuCreateHeap),
                            AM_ACPHAL(ApuAlloc),
                            AM_ACPHAL(ApuFree),
                            AM_ACPHAL(ApuMapApuAddress),
                            AM_ACPHAL(ApuMapVirtualAddress),
                            AM_ACPHAL(ApuIsVirtualAddressValid),
                            AM_ACPHAL(AcpHalAllocateShapeContexts),
                            AM_ACPHAL(AcpHalReleaseShapeContexts),
                            AM_ACPHAL(AcpHalCreate),
                        });
}

}  // namespace am::rt::hle
