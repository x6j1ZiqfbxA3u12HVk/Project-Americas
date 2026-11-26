// XCRDAPI.dll: the console's DirectStorage (plan, section 5.7). A factory opens files and makes
// status arrays and request queues; the title enqueues reads, each from a file or memory into
// memory or physical pages, optionally zlib-compressed, and polls status entries.
//
// The interfaces' slots are the ones the game calls, read from its code: factory 4 OpenFile,
// 5 CreateStatusArray, 8 SetStagingBufferSize, 10 CreateQueue; queue 3 EnqueueRequest,
// 4 EnqueueStatus, 6 Submit, 8 Close; status array 3 IsComplete, 4 GetHResult. Others log
// themselves when called.
//
// Requests of all queues run on one pool of workers, in parallel. Completion keeps queue order:
// a status entry is written once every request enqueued before it on its queue has finished,
// with the first failure since the previous status entry, or S_OK. A queue submits on its own
// when half full and blocks the title while full, as the console's does.
//
// Not yet: BCPack (texture data; plan, section 9, risk 5: its tables need a legitimate source)
// and the further transforms; such requests fail with E_NOTIMPL and are logged.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/text.h"
#include "formats/bcpack.h"
#include "formats/inflate.h"
#include "runtime/process.h"
#include "runtime/d3d12x/bindings.h"
#include "runtime/gdk/runtime.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"

namespace am::rt::storage {
namespace {

constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000A);
constexpr uint64_t kZlib = 1;
constexpr uint64_t kBcpackMask = 0xFull << 1;
constexpr uint64_t kTransformMask = 0xFFFFull << 5;  // bits 5-20: further transforms
constexpr uint64_t kPageDestination = 1ull << 21;
constexpr uint64_t kMemorySource = 1ull << 22;
constexpr uint64_t kPageSource = 1ull << 23;
constexpr uint64_t kPage = 64 * 1024;

// ---- objects ----

enum class Kind : uint32_t { Factory = 0x46534441, File, StatusArray, Queue };  // "ADSF"...

// No virtual functions: title code reads our vtable at offset 0.
struct Object {
  void* const* vtable;
  Kind kind;
  std::atomic<long> refs{1};
};
void destroy(Object* o);

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Factory: return "factory";
    case Kind::File: return "file";
    case Kind::StatusArray: return "status array";
    case Kind::Queue: return "queue";
  }
  return "?";
}

void add_ref(Object* o) { o->refs.fetch_add(1, std::memory_order_relaxed); }
void release(Object* o) {
  if (o->kind == Kind::Factory) return;  // static
  if (o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) destroy(o);
}

template <int N>
HRESULT WINAPI missing(Object* self) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) logf("dstorage: %s slot %d is not implemented", kind_name(self->kind), N);
  return E_NOTIMPL;
}
HRESULT WINAPI query_interface(Object* self, const GUID*, void** out) {
  if (!out) return E_POINTER;
  add_ref(self);
  *out = self;
  return S_OK;
}
ULONG WINAPI add_ref_method(Object* self) { return static_cast<ULONG>(self->refs.fetch_add(1) + 1); }
ULONG WINAPI release_method(Object* self) {
  if (self->kind == Kind::Factory) return 1;
  const long left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) destroy(self);
  return static_cast<ULONG>(left);
}

struct File : Object {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::string name;
  ~File() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

struct StatusArray : Object {
  uint32_t capacity = 0;
  std::unique_ptr<std::atomic<HRESULT>[]> entries;
};

// A read request as the title builds it.
struct Request {
  uint64_t options;
  void* destination;     // an address, or with kPageDestination an array of page numbers
  uint64_t page_offset;  // with kPageDestination: the low 16 bits, where in the first page it starts
  uint32_t destination_size;
  uint32_t reserved;
  const void* source;    // our file object, or with kMemorySource an address
  uint64_t offset;
  uint32_t source_size;
  uint32_t intermediate_size;
  uint64_t tag;
  const char* name;
};
static_assert(sizeof(Request) == 0x48);
static_assert(offsetof(Object, vtable) == 0);

struct QueueDesc {
  uint64_t source_type;
  uint16_t capacity;
  int8_t priority;
  const char* name;
  uint64_t unknown;  // the game sets 1
};

struct Command {
  bool status = false;
  bool submitted = false;
  enum State : uint8_t { Waiting, Running, Done } state = Waiting;
  HRESULT result = S_OK;
  Request request{};
  File* file = nullptr;          // referenced while the command lives
  StatusArray* array = nullptr;  // referenced while the command lives
  uint32_t index = 0;
};

struct Queue : Object {
  std::string name;
  uint32_t capacity = 0;
  bool closed = false;
  HRESULT failure = S_OK;  // the first since the previous status entry
  std::deque<Command> commands;
  size_t running = 0;
};

void close_queue(Queue* q);

void destroy(Object* o) {
  switch (o->kind) {
    case Kind::File: delete static_cast<File*>(o); break;
    case Kind::StatusArray: delete static_cast<StatusArray*>(o); break;
    case Kind::Queue:
      close_queue(static_cast<Queue*>(o));
      delete static_cast<Queue*>(o);
      break;
    case Kind::Factory: break;
  }
}

// ---- the scheduler ----

struct Scheduler {
  std::mutex lock;
  std::condition_variable work;      // a request to start
  std::condition_variable finished;  // a request finished (full queues, Close)
  std::vector<Queue*> queues;
  size_t next_queue = 0;
  std::vector<std::thread> workers;
};

// Totals for report.json, apart from the scheduler: reporting must not start its threads.
std::atomic<uint64_t> g_requests{0}, g_bytes{0}, g_failures{0};

Scheduler& scheduler();

void drop(Command& c) {
  if (c.file) release(c.file);
  if (c.array) release(c.array);
  c.file = nullptr;
  c.array = nullptr;
}

// Retires the queue's finished front: failures noted, status entries written in order.
void retire(Queue* q) {
  while (!q->commands.empty()) {
    Command& c = q->commands.front();
    if (!c.submitted) break;
    if (c.status) {
      c.array->entries[c.index].store(q->failure, std::memory_order_release);
      q->failure = S_OK;
    } else {
      if (c.state != Command::Done) break;
      if (FAILED(c.result) && q->failure == S_OK) q->failure = c.result;
    }
    drop(c);
    q->commands.pop_front();
  }
}

// ---- executing a request ----

thread_local std::vector<uint8_t> t_source, t_out, t_blocks;

// BCPack's constant tables: not ours and not in the repository, read once from
// <root>	oolscpack	ables.bin (AMERICAS_BCPACK_TABLES names another file). nullptr when there
// are none: BCPack requests are then refused.
const formats::bcpack::Tables* bcpack_tables() {
  static const std::unique_ptr<formats::bcpack::Tables> tables = [] {
    std::filesystem::path file;
    wchar_t named[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"AMERICAS_BCPACK_TABLES", named, MAX_PATH)) file = named;
    else if (TitleProcess* p = process(); p && !p->cache_dir().empty()) file = p->cache_dir().parent_path() / "tools" / "bcpack" / "tables.bin";
    std::string error;
    std::vector<uint8_t> bytes;
    if (std::ifstream in = file.empty() ? std::ifstream() : std::ifstream(file, std::ios::binary); in) {
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
      error = "no table file";
    }
    std::unique_ptr<formats::bcpack::Tables> t = error.empty() ? formats::bcpack::Tables::load(bytes, error) : nullptr;
    logf("dstorage: BCPack tables %s (%s)", t ? "loaded" : "unavailable", t ? narrow(file.wstring()).c_str() : error.c_str());
    return t;
  }();
  return tables.get();
}

bool read_file(File* f, uint64_t offset, uint32_t size, std::vector<uint8_t>& out, HRESULT& hr) {
  out.resize(size);
  uint32_t done = 0;
  while (done < size) {
    OVERLAPPED ov{};
    const uint64_t at = offset + done;
    ov.Offset = static_cast<DWORD>(at);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    DWORD got = 0;
    if (!ReadFile(f->handle, out.data() + done, size - done, &got, &ov)) {
      hr = HRESULT_FROM_WIN32(GetLastError());
      return false;
    }
    if (!got) {
      hr = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
      return false;
    }
    done += got;
  }
  return true;
}

// The title's memory may be bad; a fault fails the request instead of the process.
bool guarded_copy(void* to, const void* from, size_t size) {
  __try {
    std::memcpy(to, from, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void log_refused(const Request& r, const char* why) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 24) {
    logf("dstorage: request refused (%s): options 0x%s, %u -> %u bytes, %s", why, hex(r.options).c_str(), r.source_size,
         r.destination_size, r.name ? r.name : "");
  }
}

HRESULT execute(const Request& r) {
  if (r.options & (kTransformMask | kPageSource)) {
    log_refused(r, "transform not implemented yet");
    return E_NOTIMPL;
  }
  const int bc = static_cast<int>((r.options & kBcpackMask) >> 1);
  const formats::bcpack::Tables* tables = bc ? bcpack_tables() : nullptr;
  if (bc && !tables) {
    log_refused(r, "no BCPack tables (tools\bcpack\tables.bin)");
    return E_NOTIMPL;
  }
  // The source's bytes.
  const uint8_t* src = nullptr;
  HRESULT hr = S_OK;
  if (r.options & kMemorySource) {
    t_source.resize(r.source_size);
    if (!guarded_copy(t_source.data(), r.source, r.source_size)) return E_INVALIDARG;
  } else {
    auto* f = static_cast<File*>(const_cast<void*>(r.source));
    if (!f || f->kind != Kind::File) return E_INVALIDARG;
    if (!read_file(f, r.offset, r.source_size, t_source, hr)) return hr;
  }
  src = t_source.data();
  // What goes to the destination.
  std::span<const uint8_t> data(src, r.source_size);
  // The console's order: zlib first (into the intermediate size when BCPack follows), then BCPack.
  if (r.options & kZlib) {
    t_out.resize(bc ? r.intermediate_size : r.destination_size);
    const auto res = formats::inflate_zlib(data, t_out);
    // The destination full before the stream ends: what fits is kept, as the decompression
    // hardware stops at the destination's size (the game reads parts of larger streams so).
    const bool full = res.status == formats::InflateStatus::OutputFull;
    if (full) {
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 32) {
        // How long the stream really is, to tell a short destination from a wrong source.
        std::vector<uint8_t> whole(std::max<size_t>(t_out.size() * 64, size_t{16} << 20));
        const auto all = formats::inflate_zlib(data, whole);
        logf("dstorage: zlib stream longer than its destination (%u -> %zu bytes, whole stream %s %llu bytes): what fits is "
             "kept; options 0x%s, %s offset 0x%llx, intermediate %u, %s",
             r.source_size, t_out.size(), formats::inflate_status_name(all.status), static_cast<unsigned long long>(all.written),
             hex(r.options).c_str(), (r.options & kMemorySource) ? "memory" : static_cast<const File*>(r.source)->name.c_str(),
             static_cast<unsigned long long>(r.offset), r.intermediate_size, r.name ? r.name : "");
        const auto* q = reinterpret_cast<const uint64_t*>(&r);
        logf("dstorage:   request words %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
             q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8]);
      }
    }
    if (res.status != formats::InflateStatus::Ok && !full) {
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 8) {
        logf("dstorage: zlib %s: %u -> %llu of %zu bytes", formats::inflate_status_name(res.status), r.source_size,
             static_cast<unsigned long long>(res.written), t_out.size());
      }
      log_refused(r, "zlib stream does not inflate");
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    data = std::span<const uint8_t>(t_out.data(), full ? t_out.size() : res.written);
  } else if (!bc && r.source_size != r.destination_size) {
    return E_INVALIDARG;
  }
  if (bc) {
    t_blocks.resize(r.destination_size);
    const formats::bcpack::Status st = formats::bcpack::decode(bc, data, t_blocks, *tables);
    if (st != formats::bcpack::Status::Ok) {
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 8) logf("dstorage: BCPack (BC%d) %s", bc, formats::bcpack::status_name(st));
      log_refused(r, "BCPack");
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    data = t_blocks;
  }
  if (r.options & kPageDestination) {
    // AMERICAS_SKIP_PAGE_WRITES (diagnosis): page destinations are not written, the request
    // reported done.
    static const bool skip = GetEnvironmentVariableA("AMERICAS_SKIP_PAGE_WRITES", nullptr, 0) != 0;
    if (skip) return S_OK;
    const uint64_t within = r.page_offset & 0xFFFF;
    const uint64_t pages = (within + data.size() + kPage - 1) / kPage;
    std::vector<uint64_t> numbers(pages);
    if (!guarded_copy(numbers.data(), r.destination, pages * sizeof(uint64_t))) return E_INVALIDARG;
    if (!mem::title_memory().write_pages(numbers.data(), pages, within, data.data(), data.size())) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
  }
  if (!guarded_copy(r.destination, data.data(), data.size())) return E_INVALIDARG;
  // Textures over these bytes are uploaded again. (Page destinations are not reported yet:
  // their pages' virtual addresses are not known here.)
  d3d12x::notify_title_write(reinterpret_cast<uint64_t>(r.destination), data.size());
  return S_OK;
}

void worker() {
  Scheduler& s = scheduler();
  std::unique_lock<std::mutex> lock(s.lock);
  for (;;) {
    // The next submitted request waiting, queues served in turn.
    Queue* q = nullptr;
    Command* c = nullptr;
    for (size_t n = 0; n < s.queues.size() && !c; ++n) {
      Queue* candidate = s.queues[(s.next_queue + n) % s.queues.size()];
      for (Command& x : candidate->commands) {
        if (!x.submitted) break;
        if (!x.status && x.state == Command::Waiting) {
          q = candidate;
          c = &x;
          s.next_queue = (s.next_queue + n + 1) % s.queues.size();
          break;
        }
      }
    }
    if (!c) {
      s.work.wait(lock);
      continue;
    }
    c->state = Command::Running;
    ++q->running;
    const Request request = c->request;
    lock.unlock();
    HRESULT hr;
    try {
      hr = execute(request);
    } catch (const std::bad_alloc&) {
      // A request larger than the memory there is: refused, not the end of the run.
      log_refused(request, "out of memory");
      logf("dstorage: request refused for want of memory: options 0x%s, offset 0x%llx, %u -> %u bytes (intermediate %u)",
           hex(request.options).c_str(), static_cast<unsigned long long>(request.offset), request.source_size,
           request.destination_size, request.intermediate_size);
      hr = E_OUTOFMEMORY;
    }
    // A large request's buffers are let go rather than kept for the next.
    for (std::vector<uint8_t>* v : {&t_source, &t_out, &t_blocks}) {
      if (v->capacity() > (64u << 20)) std::vector<uint8_t>().swap(*v);
    }
    g_requests.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(request.destination_size, std::memory_order_relaxed);
    if (FAILED(hr)) g_failures.fetch_add(1, std::memory_order_relaxed);
    lock.lock();
    c->state = Command::Done;  // deque elements stay put: only the front is ever removed
    c->result = hr;
    --q->running;
    retire(q);
    s.finished.notify_all();
  }
}

Scheduler& scheduler() {
  static Scheduler* const s = [] {
    auto* sch = new Scheduler;
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
    for (unsigned i = 0; i < n; ++i) {
      sch->workers.emplace_back(&worker);
      SetThreadDescription(sch->workers.back().native_handle(), L"americas dstorage");
    }
    for (auto& t : sch->workers) t.detach();
    return sch;
  }();
  return *s;
}

// ---- queue methods ----

void submit_locked(Scheduler& s, Queue* q) {
  for (Command& c : q->commands) c.submitted = true;
  retire(q);
  s.work.notify_all();
  s.finished.notify_all();
}

void enqueue(Queue* q, Command c) {
  Scheduler& s = scheduler();
  std::unique_lock<std::mutex> lock(s.lock);
  // One slot stays free, as the console's count of empty slots says.
  s.finished.wait(lock, [&] { return q->closed || q->commands.size() + 1 < std::max<uint32_t>(q->capacity, 2); });
  if (q->closed) {
    drop(c);
    return;
  }
  q->commands.push_back(c);
  size_t unsubmitted = 0;
  for (const Command& x : q->commands) unsubmitted += !x.submitted;
  if (unsubmitted >= std::max<uint32_t>(q->capacity / 2, 1)) submit_locked(s, q);
}

void WINAPI queue_enqueue_request(Queue* q, const Request* r) {
  if (!r) return;
  Command c;
  c.request = *r;
  if (!(r->options & kMemorySource)) {
    auto* f = static_cast<File*>(const_cast<void*>(r->source));
    if (f && f->kind == Kind::File) {
      add_ref(f);
      c.file = f;
    }
  }
  enqueue(q, c);
}
void WINAPI queue_enqueue_status(Queue* q, StatusArray* a, uint32_t index) {
  if (!a || a->kind != Kind::StatusArray || index >= a->capacity) return;
  a->entries[index].store(kPending, std::memory_order_release);
  add_ref(a);
  Command c;
  c.status = true;
  c.array = a;
  c.index = index;
  enqueue(q, c);
}
void WINAPI queue_submit(Queue* q) {
  Scheduler& s = scheduler();
  std::lock_guard<std::mutex> lock(s.lock);
  submit_locked(s, q);
}
// Ends the queue: what was not started never completes; returns once what runs has finished.
void close_queue(Queue* q) {
  Scheduler& s = scheduler();
  std::unique_lock<std::mutex> lock(s.lock);
  if (q->closed) return;
  q->closed = true;
  for (Command& c : q->commands) {
    if (c.state == Command::Waiting) c.state = Command::Done, c.result = E_ABORT;
  }
  s.finished.wait(lock, [&] { return q->running == 0; });
  for (Command& c : q->commands) drop(c);
  q->commands.clear();
  s.queues.erase(std::remove(s.queues.begin(), s.queues.end(), q), s.queues.end());
  s.next_queue = 0;
  s.finished.notify_all();
}

void WINAPI queue_close(Queue* q) { close_queue(q); }

bool WINAPI status_is_complete(StatusArray* a, uint32_t index) {
  return a && index < a->capacity && a->entries[index].load(std::memory_order_acquire) != kPending;
}
HRESULT WINAPI status_result(StatusArray* a, uint32_t index) {
  if (!a || index >= a->capacity) return E_INVALIDARG;
  return a->entries[index].load(std::memory_order_acquire);
}

#define AM_SLOT(f) reinterpret_cast<void*>(&f)
void* const kFileVtable[] = {AM_SLOT(query_interface), AM_SLOT(add_ref_method), AM_SLOT(release_method),
                             AM_SLOT(missing<3>), AM_SLOT(missing<4>), AM_SLOT(missing<5>), AM_SLOT(missing<6>),
                             AM_SLOT(missing<7>)};
void* const kStatusVtable[] = {AM_SLOT(query_interface), AM_SLOT(add_ref_method), AM_SLOT(release_method),
                               AM_SLOT(status_is_complete), AM_SLOT(status_result), AM_SLOT(missing<5>),
                               AM_SLOT(missing<6>)};
void* const kQueueVtable[] = {AM_SLOT(query_interface), AM_SLOT(add_ref_method), AM_SLOT(release_method),
                              AM_SLOT(queue_enqueue_request), AM_SLOT(queue_enqueue_status), AM_SLOT(missing<5>),
                              AM_SLOT(queue_submit), AM_SLOT(missing<7>), AM_SLOT(queue_close), AM_SLOT(missing<9>),
                              AM_SLOT(missing<10>), AM_SLOT(missing<11>)};

// ---- the factory ----

HRESULT WINAPI factory_open_file(Object*, const wchar_t* path, const GUID*, void** out) {
  if (!path || !out) return E_INVALIDARG;
  *out = nullptr;
  HANDLE h = files::open_for_reading(path);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    logf("dstorage: cannot open %s (error %lu)", narrow(path).c_str(), error);
    return HRESULT_FROM_WIN32(error);
  }
  auto* f = new File;
  f->vtable = kFileVtable;
  f->kind = Kind::File;
  f->handle = h;
  f->name = narrow(path);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 64) logf("dstorage: opened %s", f->name.c_str());
  *out = f;
  return S_OK;
}
HRESULT WINAPI factory_create_status_array(Object*, uint32_t capacity, const char* name, const GUID*, void** out) {
  if (!out || !capacity) return E_INVALIDARG;
  auto* a = new StatusArray;
  a->vtable = kStatusVtable;
  a->kind = Kind::StatusArray;
  a->capacity = capacity;
  a->entries = std::make_unique<std::atomic<HRESULT>[]>(capacity);
  for (uint32_t i = 0; i < capacity; ++i) a->entries[i].store(S_OK);
  logf("dstorage: status array of %u (%s)", capacity, name ? name : "");
  *out = a;
  return S_OK;
}
HRESULT WINAPI factory_set_staging_buffer_size(Object*, uint32_t size) {
  logf("dstorage: staging buffer %s", format_bytes(size).c_str());
  return S_OK;
}
HRESULT WINAPI factory_create_queue(Object*, const QueueDesc* desc, const GUID*, void** out) {
  if (!desc || !out) return E_INVALIDARG;
  auto* q = new Queue;
  q->vtable = kQueueVtable;
  q->kind = Kind::Queue;
  q->capacity = std::max<uint32_t>(desc->capacity, 2);
  q->name = desc->name ? desc->name : "";
  Scheduler& s = scheduler();
  {
    std::lock_guard<std::mutex> lock(s.lock);
    s.queues.push_back(q);
  }
  logf("dstorage: queue %s (source type %llu, capacity %u, priority %d)", q->name.c_str(),
       static_cast<unsigned long long>(desc->source_type), q->capacity, desc->priority);
  *out = q;
  return S_OK;
}

void* const kFactoryVtable[] = {
    AM_SLOT(query_interface), AM_SLOT(add_ref_method), AM_SLOT(release_method), AM_SLOT(missing<3>),
    AM_SLOT(factory_open_file), AM_SLOT(factory_create_status_array), AM_SLOT(missing<6>), AM_SLOT(missing<7>),
    AM_SLOT(factory_set_staging_buffer_size), AM_SLOT(missing<9>), AM_SLOT(factory_create_queue), AM_SLOT(missing<11>),
    AM_SLOT(missing<12>), AM_SLOT(missing<13>), AM_SLOT(missing<14>), AM_SLOT(missing<15>)};
#undef AM_SLOT

struct Factory : Object {};
Factory& factory() {
  static Factory* const f = [] {
    auto* x = new Factory;
    x->vtable = kFactoryVtable;
    x->kind = Kind::Factory;
    return x;
  }();
  return *f;
}

constexpr GUID kFactoryIid = gdk::guid("62959d11-d45f-4884-a0a3-f7862ea243ae");

HRESULT WINAPI my_DStorageGetFactory(const GUID* iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!iid || !IsEqualGUID(*iid, kFactoryIid)) {
    logf("dstorage: DStorageGetFactory for an interface we do not know");
    return E_NOINTERFACE;
  }
  *out = &factory();
  return S_OK;
}

}  // namespace

void write_storage_report(JsonWriter& j) {
  j.begin_object().field("requests", g_requests.load()).field("bytes", g_bytes.load()).field("failures", g_failures.load());
  j.end_object();
}

void add_storage(std::vector<hle::Function>& out) {
  out.push_back(hle::Function{"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&my_DStorageGetFactory)});
}

}  // namespace am::rt::storage
