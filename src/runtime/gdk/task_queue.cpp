#include "runtime/gdk/task_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace am::rt::gdk {
namespace {

constexpr uint32_t kQueueMagic = 0x51544D41;  // "AMTQ"
constexpr uint32_t kPortMagic = 0x50544D41;   // "AMTP"
constexpr uint32_t kBlockMagic = 0x42414D41;  // "AMAB"
constexpr HRESULT kIllegalMethodCall = static_cast<HRESULT>(0x8000000E);

struct Entry {
  TaskCallback callback;
  void* context;
  TaskQueue* via;  // the queue it was submitted through; holds a reference
};

}  // namespace

struct TaskQueuePort {
  uint32_t magic = kPortMagic;
  DispatchMode mode = DispatchMode::Manual;
  std::atomic<long> refs{1};  // queues using it, plus pool callbacks on their way
  std::mutex lock;
  std::condition_variable ready_cv;
  std::deque<Entry> ready;
  bool serial_running = false;
};

struct TaskQueue {
  struct Monitor {
    uint64_t token;
    void* context;
    MonitorCallback callback;
  };
  struct Delayed;
  struct Waiter;

  uint32_t magic = kQueueMagic;
  std::atomic<long> refs{1};  // handles, plus entries, timers, waiters and calls that use it
  TaskQueuePort* ports[2] = {};
  std::mutex lock;
  std::condition_variable idle_cv;
  long in_flight = 0;  // entries submitted through it and not yet finished
  bool terminated = false;
  std::vector<Monitor> monitors;
  std::vector<Delayed*> delayed;
  std::vector<Waiter*> waiters;
};

struct TaskQueue::Delayed {
  TaskQueue* queue;
  PortId port;
  Entry entry;
  PTP_TIMER timer = nullptr;
  std::atomic<bool> claimed{false};
};

struct TaskQueue::Waiter {
  uint64_t token;
  TaskQueue* queue;
  PortId port;
  void* context;
  TaskCallback callback;
  PTP_WAIT wait = nullptr;
  std::atomic<bool> claimed{false};
};

namespace {

std::atomic<uint64_t> g_next_token{1};

bool valid(const TaskQueue* q) {
  __try {
    return q && q->magic == kQueueMagic;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool valid(const TaskQueuePort* p) {
  __try {
    return p && p->magic == kPortMagic;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void add_ref(TaskQueue* q) { q->refs.fetch_add(1, std::memory_order_relaxed); }
void release_port(TaskQueuePort* p) {
  if (p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    p->magic = 0;
    delete p;
  }
}
void release(TaskQueue* q) {
  if (q->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    q->magic = 0;
    for (TaskQueuePort* p : q->ports) release_port(p);
    delete q;
  }
}

// ---- the process queue ----

std::mutex g_process_lock;
TaskQueue* g_process = nullptr;
bool g_process_made = false;

TaskQueue* acquire_process_queue() {
  std::lock_guard<std::mutex> lock(g_process_lock);
  if (!g_process_made) {
    g_process_made = true;
    queue_create(DispatchMode::ThreadPool, DispatchMode::ThreadPool, &g_process);
  }
  if (g_process) add_ref(g_process);
  return g_process;
}

// A queue to use for one operation, referenced: the given one, or the process's for null.
TaskQueue* acquire(TaskQueue* q) {
  if (!q) return acquire_process_queue();
  if (!valid(q)) return nullptr;
  add_ref(q);
  return q;
}

// ---- running entries ----

void finish(const Entry& e) {
  TaskQueue* q = e.via;
  {
    std::lock_guard<std::mutex> lock(q->lock);
    if (--q->in_flight == 0) q->idle_cv.notify_all();
  }
  release(q);
}

void run(const Entry& e, bool canceled) {
  e.callback(e.context, canceled);
  finish(e);
}

void CALLBACK pool_one(PTP_CALLBACK_INSTANCE, void* context) {
  auto* p = static_cast<TaskQueuePort*>(context);
  Entry e{};
  bool have = false;
  {
    std::lock_guard<std::mutex> lock(p->lock);
    if (!p->ready.empty()) {
      e = p->ready.front();
      p->ready.pop_front();
      have = true;
    }
  }
  if (have) run(e, false);  // otherwise termination took it
  release_port(p);
}

void CALLBACK pool_serial(PTP_CALLBACK_INSTANCE, void* context) {
  auto* p = static_cast<TaskQueuePort*>(context);
  for (;;) {
    Entry e{};
    {
      std::lock_guard<std::mutex> lock(p->lock);
      if (p->ready.empty()) {
        p->serial_running = false;
        break;
      }
      e = p->ready.front();
      p->ready.pop_front();
    }
    run(e, false);
  }
  release_port(p);
}

void notify_monitors(TaskQueue* q, PortId port) {
  std::vector<TaskQueue::Monitor> monitors;
  {
    std::lock_guard<std::mutex> lock(q->lock);
    monitors = q->monitors;
  }
  for (const TaskQueue::Monitor& m : monitors) m.callback(m.context, q, port);
}

// Puts a referenced entry on `port` of `q`, by the port's dispatch mode.
void enqueue(TaskQueue* q, PortId port, const Entry& e) {
  TaskQueuePort* p = q->ports[static_cast<int>(port)];
  switch (p->mode) {
    case DispatchMode::Immediate:
      run(e, false);
      return;
    case DispatchMode::Manual: {
      {
        std::lock_guard<std::mutex> lock(p->lock);
        p->ready.push_back(e);
      }
      p->ready_cv.notify_one();
      break;
    }
    case DispatchMode::ThreadPool: {
      {
        std::lock_guard<std::mutex> lock(p->lock);
        p->ready.push_back(e);
      }
      p->refs.fetch_add(1, std::memory_order_relaxed);
      if (!TrySubmitThreadpoolCallback(&pool_one, p, nullptr)) pool_one(nullptr, p);
      break;
    }
    case DispatchMode::SerializedThreadPool: {
      bool start = false;
      {
        std::lock_guard<std::mutex> lock(p->lock);
        p->ready.push_back(e);
        start = !p->serial_running;
        p->serial_running = true;
      }
      if (start) {
        p->refs.fetch_add(1, std::memory_order_relaxed);
        if (!TrySubmitThreadpoolCallback(&pool_serial, p, nullptr)) pool_serial(nullptr, p);
      }
      break;
    }
  }
  notify_monitors(q, port);
}

// An entry for `q`: counted in flight and holding a reference. False once `q` is terminated.
bool make_entry(TaskQueue* q, void* context, TaskCallback callback, Entry* out) {
  std::lock_guard<std::mutex> lock(q->lock);
  if (q->terminated) return false;
  ++q->in_flight;
  add_ref(q);
  *out = Entry{callback, context, q};
  return true;
}

void CALLBACK delayed_fired(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER timer) {
  auto* d = static_cast<TaskQueue::Delayed*>(context);
  if (d->claimed.exchange(true)) return;  // termination has it
  TaskQueue* q = d->queue;
  {
    std::lock_guard<std::mutex> lock(q->lock);
    q->delayed.erase(std::remove(q->delayed.begin(), q->delayed.end(), d), q->delayed.end());
  }
  enqueue(q, d->port, d->entry);
  CloseThreadpoolTimer(timer);
  release(q);
  delete d;
}

void CALLBACK waiter_fired(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
  auto* w = static_cast<TaskQueue::Waiter*>(context);
  Entry e{};
  if (make_entry(w->queue, w->context, w->callback, &e)) enqueue(w->queue, w->port, e);
  // One call per registration; XTaskQueueUnregisterWaiter frees it.
}

void cancel_waiter(TaskQueue::Waiter* w) {
  SetThreadpoolWait(w->wait, nullptr, nullptr);
  WaitForThreadpoolWaitCallbacks(w->wait, TRUE);
  CloseThreadpoolWait(w->wait);
  release(w->queue);
  delete w;
}

thread_local bool t_time_sensitive = false;

}  // namespace

// ---- queues ----

HRESULT queue_create(DispatchMode work, DispatchMode completion, TaskQueue** out) {
  if (!out || static_cast<uint32_t>(work) > 3 || static_cast<uint32_t>(completion) > 3) return E_INVALIDARG;
  auto* q = new TaskQueue;
  q->ports[0] = new TaskQueuePort;
  q->ports[0]->mode = work;
  q->ports[1] = new TaskQueuePort;
  q->ports[1]->mode = completion;
  *out = q;
  return S_OK;
}

HRESULT queue_create_composite(TaskQueuePort* work, TaskQueuePort* completion, TaskQueue** out) {
  if (!out || !valid(work) || !valid(completion)) return E_INVALIDARG;
  auto* q = new TaskQueue;
  work->refs.fetch_add(1, std::memory_order_relaxed);
  completion->refs.fetch_add(1, std::memory_order_relaxed);
  q->ports[0] = work;
  q->ports[1] = completion;
  *out = q;
  return S_OK;
}

HRESULT queue_get_port(TaskQueue* queue, PortId port, TaskQueuePort** out) {
  if (!out || static_cast<uint32_t>(port) > 1) return E_INVALIDARG;
  TaskQueue* q = acquire(queue);
  if (!q) return E_INVALIDARG;
  *out = q->ports[static_cast<int>(port)];  // lives as long as a queue using it
  release(q);
  return S_OK;
}

HRESULT queue_duplicate(TaskQueue* queue, TaskQueue** out) {
  if (!out || !valid(queue)) return E_INVALIDARG;
  add_ref(queue);
  *out = queue;
  return S_OK;
}

void queue_close(TaskQueue* queue) {
  if (valid(queue)) release(queue);
}

bool queue_dispatch(TaskQueue* queue, PortId port, uint32_t timeout_ms) {
  if (static_cast<uint32_t>(port) > 1) return false;
  TaskQueue* q = acquire(queue);
  if (!q) return false;
  TaskQueuePort* p = q->ports[static_cast<int>(port)];
  Entry e{};
  bool have = false;
  if (p->mode == DispatchMode::Manual) {
    std::unique_lock<std::mutex> lock(p->lock);
    const auto has_work = [p] { return !p->ready.empty(); };
    if (timeout_ms == INFINITE) p->ready_cv.wait(lock, has_work);
    else if (timeout_ms) p->ready_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), has_work);
    if (!p->ready.empty()) {
      e = p->ready.front();
      p->ready.pop_front();
      have = true;
    }
  }
  release(q);
  if (have) run(e, false);
  return have;
}

HRESULT queue_submit(TaskQueue* queue, PortId port, uint32_t delay_ms, void* context, TaskCallback callback) {
  if (!callback || static_cast<uint32_t>(port) > 1) return E_INVALIDARG;
  TaskQueue* q = acquire(queue);
  if (!q) return E_INVALIDARG;
  Entry e{};
  if (!make_entry(q, context, callback, &e)) {
    release(q);
    return E_ABORT;
  }
  if (delay_ms == 0) {
    enqueue(q, port, e);
    release(q);
    return S_OK;
  }
  auto* d = new TaskQueue::Delayed{q, port, e};  // keeps the reference acquire() took
  d->timer = CreateThreadpoolTimer(&delayed_fired, d, nullptr);
  if (!d->timer) {
    delete d;
    run(e, true);
    release(q);
    return HRESULT_FROM_WIN32(GetLastError());
  }
  {
    std::lock_guard<std::mutex> lock(q->lock);
    q->delayed.push_back(d);
  }
  ULARGE_INTEGER due;
  due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(delay_ms) * 10000);  // relative, 100 ns units
  FILETIME ft{due.LowPart, due.HighPart};
  SetThreadpoolTimer(d->timer, &ft, 0, 0);
  return S_OK;
}

HRESULT queue_register_waiter(TaskQueue* queue, PortId port, HANDLE handle, void* context, TaskCallback callback,
                              uint64_t* token) {
  if (!callback || !handle || !token || static_cast<uint32_t>(port) > 1) return E_INVALIDARG;
  TaskQueue* q = acquire(queue);
  if (!q) return E_INVALIDARG;
  auto* w = new TaskQueue::Waiter{g_next_token.fetch_add(1), q, port, context, callback};
  w->wait = CreateThreadpoolWait(&waiter_fired, w, nullptr);
  if (!w->wait) {
    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    delete w;
    release(q);
    return hr;
  }
  {
    std::lock_guard<std::mutex> lock(q->lock);
    if (q->terminated) {
      CloseThreadpoolWait(w->wait);
      delete w;
      release(q);
      return E_ABORT;
    }
    q->waiters.push_back(w);
  }
  *token = w->token;
  SetThreadpoolWait(w->wait, handle, nullptr);
  return S_OK;
}

void queue_unregister_waiter(TaskQueue* queue, uint64_t token) {
  TaskQueue* q = acquire(queue);
  if (!q) return;
  TaskQueue::Waiter* found = nullptr;
  {
    std::lock_guard<std::mutex> lock(q->lock);
    for (auto it = q->waiters.begin(); it != q->waiters.end(); ++it) {
      if ((*it)->token == token) {
        found = *it;
        q->waiters.erase(it);
        break;
      }
    }
  }
  if (found) cancel_waiter(found);
  release(q);
}

HRESULT queue_register_monitor(TaskQueue* queue, void* context, MonitorCallback callback, uint64_t* token) {
  if (!callback || !token || !valid(queue)) return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(queue->lock);
  *token = g_next_token.fetch_add(1);
  queue->monitors.push_back({*token, context, callback});
  return S_OK;
}

void queue_unregister_monitor(TaskQueue* queue, uint64_t token) {
  if (!valid(queue)) return;
  std::lock_guard<std::mutex> lock(queue->lock);
  auto& m = queue->monitors;
  m.erase(std::remove_if(m.begin(), m.end(), [token](const TaskQueue::Monitor& x) { return x.token == token; }), m.end());
}

namespace {

struct Termination {
  TaskQueue* queue;
  void* context;
  TerminatedCallback callback;
};

// Cancels what is pending on `q`, waits for what runs, then reports.
void terminate_now(const Termination& t) {
  TaskQueue* q = t.queue;
  std::vector<TaskQueue::Delayed*> delayed;
  std::vector<TaskQueue::Waiter*> waiters;
  {
    std::lock_guard<std::mutex> lock(q->lock);
    delayed.swap(q->delayed);
    waiters.swap(q->waiters);
  }
  for (TaskQueue::Delayed* d : delayed) {
    if (d->claimed.exchange(true)) continue;  // firing: it goes through the port, below
    SetThreadpoolTimer(d->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(d->timer, TRUE);
    CloseThreadpoolTimer(d->timer);
    run(d->entry, true);
    release(d->queue);
    delete d;
  }
  for (TaskQueue::Waiter* w : waiters) cancel_waiter(w);
  for (TaskQueuePort* p : q->ports) {
    std::vector<Entry> mine;
    {
      std::lock_guard<std::mutex> lock(p->lock);
      auto keep = std::stable_partition(p->ready.begin(), p->ready.end(), [q](const Entry& e) { return e.via != q; });
      mine.assign(keep, p->ready.end());
      p->ready.erase(keep, p->ready.end());
    }
    for (const Entry& e : mine) run(e, true);
  }
  {
    std::unique_lock<std::mutex> lock(q->lock);
    q->idle_cv.wait(lock, [q] { return q->in_flight == 0; });
  }
  if (t.callback) t.callback(t.context);
  release(q);
}

void CALLBACK terminate_later(PTP_CALLBACK_INSTANCE, void* context) {
  auto* t = static_cast<Termination*>(context);
  terminate_now(*t);
  delete t;
}

}  // namespace

HRESULT queue_terminate(TaskQueue* queue, bool wait, void* context, TerminatedCallback callback) {
  if (!valid(queue)) return E_INVALIDARG;
  {
    std::lock_guard<std::mutex> lock(queue->lock);
    if (queue->terminated) return kIllegalMethodCall;
    queue->terminated = true;
  }
  add_ref(queue);
  const Termination t{queue, context, callback};
  if (wait) {
    terminate_now(t);
    return S_OK;
  }
  auto* later = new Termination(t);
  if (!TrySubmitThreadpoolCallback(&terminate_later, later, nullptr)) terminate_later(nullptr, later);
  return S_OK;
}

bool process_queue_get(TaskQueue** out) {
  if (!out) return false;
  *out = acquire_process_queue();
  return *out != nullptr;
}

void process_queue_set(TaskQueue* queue) {
  if (queue && !valid(queue)) return;
  if (queue) add_ref(queue);
  TaskQueue* old = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_process_lock);
    g_process_made = true;
    old = g_process;
    g_process = queue;
  }
  if (old) release(old);
}

HRESULT thread_set_time_sensitive(bool sensitive) {
  t_time_sensitive = sensitive;
  return S_OK;
}
bool thread_is_time_sensitive() { return t_time_sensitive; }

// ---- XAsync ----

namespace {

struct AsyncState {
  std::atomic<long> refs{1};  // the block's, plus each callback and caller using it
  AsyncBlock* block = nullptr;
  AsyncProvider provider = nullptr;
  void* context = nullptr;
  const void* identity = nullptr;
  TaskQueue* queue = nullptr;  // referenced
  HANDLE done = nullptr;       // set once the completion routine has run
  std::mutex lock;
  HRESULT status = kPending;
  size_t required = 0;
  bool completed = false;
  bool consumed = false;  // the result was read (or there is none): Cleanup has run or is running
};

// What the block's `internal` bytes hold.
struct BlockInternal {
  AsyncState* state;
  HRESULT status;  // the final status, kept once the state is gone
  uint32_t magic;
};
static_assert(sizeof(BlockInternal) <= sizeof(AsyncBlock::internal));

std::mutex g_blocks;  // guards every block's `internal`
thread_local AsyncState* t_completing = nullptr;

BlockInternal* internal_of(AsyncBlock* block) { return reinterpret_cast<BlockInternal*>(block->internal); }

void release(AsyncState* s) {
  if (s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (s->done) CloseHandle(s->done);
    if (s->queue) gdk::release(s->queue);
    delete s;
  }
}

// The block's state, referenced; null if it has none (never begun, or finished).
AsyncState* acquire(AsyncBlock* block, HRESULT* final_status) {
  std::lock_guard<std::mutex> lock(g_blocks);
  BlockInternal* bi = internal_of(block);
  if (bi->magic != kBlockMagic) {
    if (final_status) *final_status = E_INVALIDARG;
    return nullptr;
  }
  if (final_status) *final_status = bi->status;
  if (!bi->state) return nullptr;
  bi->state->refs.fetch_add(1, std::memory_order_relaxed);
  return bi->state;
}

// The block lets go of its state, keeping the final status.
void detach(AsyncState* s, HRESULT status) {
  bool was = false;
  {
    std::lock_guard<std::mutex> lock(g_blocks);
    BlockInternal* bi = internal_of(s->block);
    if (bi->magic == kBlockMagic && bi->state == s) {
      bi->state = nullptr;
      bi->status = status;
      was = true;
    }
  }
  if (was) release(s);
}

HRESULT call(AsyncState* s, AsyncOp op, size_t size = 0, void* buffer = nullptr) {
  const AsyncProviderData data{s->block, size, buffer, s->context};
  return s->provider(op, &data);
}

void CALLBACK completion_routine(void* context, bool /*canceled*/) {
  auto* s = static_cast<AsyncState*>(context);
  AsyncCompletion routine = s->block->callback;
  AsyncState* outer = t_completing;
  t_completing = s;
  routine(s->block);  // delivered even when the queue is terminating
  t_completing = outer;
  SetEvent(s->done);
  release(s);
}

void complete(AsyncState* s, HRESULT result, size_t required) {
  if (result == kPending) return;
  AsyncCompletion routine = nullptr;
  bool cleanup = false;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    if (s->completed) return;
    s->completed = true;
    s->status = result;
    s->required = SUCCEEDED(result) ? required : 0;
    cleanup = s->required == 0;
    if (cleanup) s->consumed = true;
    routine = s->block->callback;
  }
  if (cleanup) {
    call(s, AsyncOp::Cleanup);
    detach(s, result);
  } else {
    std::lock_guard<std::mutex> lock(g_blocks);
    if (internal_of(s->block)->state == s) internal_of(s->block)->status = result;
  }
  if (!routine) {
    SetEvent(s->done);
    return;
  }
  s->refs.fetch_add(1, std::memory_order_relaxed);
  Entry e{};
  if (make_entry(s->queue, s, &completion_routine, &e)) enqueue(s->queue, PortId::Completion, e);
  else completion_routine(s, true);
}

void CALLBACK do_work(void* context, bool canceled) {
  auto* s = static_cast<AsyncState*>(context);
  bool skip = false;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    skip = s->completed;
  }
  if (canceled) complete(s, E_ABORT, 0);
  else if (!skip) {
    const HRESULT hr = call(s, AsyncOp::DoWork);
    if (hr != kPending) complete(s, hr, 0);
  }
  release(s);
}

int g_run_identity;  // XAsyncRun's identity: its address

HRESULT CALLBACK run_provider(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork:
      return reinterpret_cast<AsyncWork>(data->context)(data->async);
    default:
      return S_OK;
  }
}

}  // namespace

HRESULT async_begin(AsyncBlock* block, void* context, const void* identity, const char* /*name*/,
                    AsyncProvider provider) {
  if (!block || !provider) return E_INVALIDARG;
  {
    std::lock_guard<std::mutex> lock(g_blocks);
    const BlockInternal* bi = internal_of(block);
    if (bi->magic == kBlockMagic && bi->state) return E_INVALIDARG;  // still in use
  }
  TaskQueue* q = gdk::acquire(block->queue);
  if (!q) return E_INVALIDARG;
  auto* s = new AsyncState;
  s->block = block;
  s->provider = provider;
  s->context = context;
  s->identity = identity;
  s->queue = q;
  s->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  {
    std::lock_guard<std::mutex> lock(g_blocks);
    *internal_of(block) = BlockInternal{s, kPending, kBlockMagic};
  }
  const HRESULT hr = call(s, AsyncOp::Begin);
  if (FAILED(hr)) {
    // The call fails, not XAsyncBegin: the block completes with the error.
    s->refs.fetch_add(1, std::memory_order_relaxed);
    complete(s, hr, 0);
    release(s);
  }
  return S_OK;
}

HRESULT async_schedule(AsyncBlock* block, uint32_t delay_ms) {
  if (!block) return E_INVALIDARG;
  AsyncState* s = acquire(block, nullptr);
  if (!s) return E_INVALIDARG;
  s->refs.fetch_add(1, std::memory_order_relaxed);  // the work callback's
  const HRESULT hr = queue_submit(s->queue, PortId::Work, delay_ms, s, &do_work);
  if (FAILED(hr)) release(s);
  release(s);
  return hr;
}

void async_complete(AsyncBlock* block, HRESULT result, size_t required_size) {
  if (!block) return;
  AsyncState* s = acquire(block, nullptr);
  if (!s) return;
  complete(s, result, required_size);
  release(s);
}

HRESULT async_get_status(AsyncBlock* block, bool wait) {
  if (!block) return E_INVALIDARG;
  HRESULT final_status = E_INVALIDARG;
  AsyncState* s = acquire(block, &final_status);
  if (!s) return final_status;
  if (wait && t_completing != s) WaitForSingleObject(s->done, INFINITE);
  HRESULT hr;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    hr = s->completed ? s->status : kPending;
  }
  release(s);
  return hr;
}

HRESULT async_get_result_size(AsyncBlock* block, size_t* size) {
  if (!block || !size) return E_INVALIDARG;
  HRESULT final_status = E_INVALIDARG;
  AsyncState* s = acquire(block, &final_status);
  if (!s) {
    *size = 0;
    return final_status;
  }
  HRESULT hr;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    hr = s->completed ? s->status : kPending;
    *size = s->completed ? s->required : 0;
  }
  release(s);
  return hr;
}

HRESULT async_get_result(AsyncBlock* block, const void* identity, size_t size, void* buffer, size_t* used) {
  if (!block) return E_INVALIDARG;
  if (used) *used = 0;
  HRESULT final_status = E_INVALIDARG;
  AsyncState* s = acquire(block, &final_status);
  if (!s) return final_status;  // finished: its status (no result left to read)
  HRESULT hr = S_OK;
  size_t required = 0;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    if (!s->completed) hr = kPending;
    else if (FAILED(s->status)) hr = s->status;
    else if (identity != s->identity) hr = E_INVALIDARG;
    else if (s->required > size || (s->required && !buffer)) hr = kNotSufficientBuffer;
    else if (s->consumed) hr = kIllegalMethodCall;
    else {
      s->consumed = true;
      required = s->required;
    }
  }
  if (hr != S_OK || !required) {
    release(s);
    return hr;
  }
  hr = call(s, AsyncOp::GetResult, required, buffer);
  call(s, AsyncOp::Cleanup);
  if (used && SUCCEEDED(hr)) *used = required;
  detach(s, SUCCEEDED(hr) ? S_OK : hr);
  release(s);
  return hr;
}

void async_cancel(AsyncBlock* block) {
  if (!block) return;
  AsyncState* s = acquire(block, nullptr);
  if (!s) return;
  bool pending = false;
  {
    std::lock_guard<std::mutex> lock(s->lock);
    pending = !s->completed;
  }
  // Cancelling is the provider's to do: one that supports it completes with E_ABORT.
  if (pending) call(s, AsyncOp::Cancel);
  release(s);
}

HRESULT async_run(AsyncBlock* block, AsyncWork work) {
  if (!work) return E_INVALIDARG;
  return async_begin(block, reinterpret_cast<void*>(work), &g_run_identity, "XAsyncRun", &run_provider);
}

}  // namespace am::rt::gdk
