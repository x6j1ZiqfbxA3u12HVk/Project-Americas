// XGameRuntime: QueryApiImpl and the XTaskQueue / XAsync core every other area builds on.

#include <windows.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "runtime/gdk/runtime.h"
#include "runtime/gdk/task_queue.h"
#include "test.h"

namespace {

using namespace am::rt::gdk;

struct Log {
  std::vector<int> order;
  int canceled = 0;
};
struct Call {
  Log* log;
  int id;
};

void CALLBACK record(void* context, bool canceled) {
  auto* c = static_cast<Call*>(context);
  c->log->order.push_back(c->id);
  if (canceled) ++c->log->canceled;
}

std::atomic<int> g_count{0};
std::atomic<int> g_concurrent{0};
std::atomic<int> g_max_concurrent{0};
void CALLBACK count(void* event, bool) {
  const int now = ++g_concurrent;
  int seen = g_max_concurrent.load();
  while (now > seen && !g_max_concurrent.compare_exchange_weak(seen, now)) {
  }
  Sleep(1);
  --g_concurrent;
  if (++g_count == 20) SetEvent(static_cast<HANDLE>(event));
}

void CALLBACK terminated(void* flag) { *static_cast<bool*>(flag) = true; }

int g_monitored = 0;
void CALLBACK monitor(void*, TaskQueue*, PortId port) { g_monitored += port == PortId::Work; }

// A provider that schedules its work, and completes with 4 bytes of result.
HRESULT CALLBACK four_bytes(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork:
      async_complete(data->async, S_OK, 4);
      return S_OK;
    case AsyncOp::GetResult:
      std::memcpy(data->buffer, "done", 4);
      return S_OK;
    default:
      return S_OK;
  }
}
HRESULT CALLBACK fails_to_begin(AsyncOp op, const AsyncProviderData*) { return op == AsyncOp::Begin ? E_FAIL : S_OK; }
HRESULT CALLBACK does_nothing(AsyncOp, const AsyncProviderData*) { return S_OK; }
HRESULT CALLBACK aborts_on_cancel(AsyncOp op, const AsyncProviderData* data) {
  if (op == AsyncOp::Cancel) async_complete(data->async, E_ABORT, 0);
  return S_OK;
}
HRESULT CALLBACK work_returns_7(AsyncBlock*) { return 7; }

int g_completions = 0;
void CALLBACK completed(AsyncBlock*) { ++g_completions; }

}  // namespace

TEST_CASE(runtime_guid_and_query) {
  const GUID g = guid("073b7dcb-1fcf-4030-94be-e3c9eb623428");
  CHECK(g.Data1 == 0x073b7dcb && g.Data2 == 0x1fcf && g.Data3 == 0x4030 && g.Data4[0] == 0x94 && g.Data4[7] == 0x28);
  void* object = reinterpret_cast<void*>(1);
  CHECK(query_api(&g, &g, &object) == S_OK && object);
  // Slot 12 is XTaskQueueCreate, called with the object first.
  using Create = HRESULT(WINAPI*)(void*, DispatchMode, DispatchMode, TaskQueue**);
  const auto create = reinterpret_cast<Create>((*static_cast<void***>(object))[12]);
  TaskQueue* q = nullptr;
  CHECK(create(object, DispatchMode::Manual, DispatchMode::Manual, &q) == S_OK && q);
  queue_close(q);
  const GUID store = guid("0dd112ac-7c24-448c-b92b-3960fb5bd30c");  // catalogued, nothing implemented yet
  const GUID store_iid = guid("b09d803c-2414-4a05-82c6-66dfdc9e9a44");
  CHECK(query_api(&store, &store_iid, &object) == E_NOINTERFACE && object == nullptr);
  const GUID unknown = guid("00000000-0000-0000-0000-000000000001");
  CHECK(query_api(&unknown, &unknown, &object) == E_NOINTERFACE);
}

TEST_CASE(task_queue_manual_immediate_and_delayed) {
  TaskQueue* q = nullptr;
  CHECK(queue_create(DispatchMode::Manual, DispatchMode::Immediate, &q) == S_OK);
  Log log;
  Call a{&log, 1}, b{&log, 2}, c{&log, 3};
  CHECK(queue_submit(q, PortId::Work, 0, &a, &record) == S_OK);
  CHECK(queue_submit(q, PortId::Work, 0, &b, &record) == S_OK);
  CHECK(log.order.empty());  // Manual: nothing runs until dispatched
  CHECK(queue_submit(q, PortId::Completion, 0, &c, &record) == S_OK);
  CHECK(log.order.size() == 1 && log.order[0] == 3);  // Immediate: on the submitting thread
  CHECK(queue_dispatch(q, PortId::Work, 0) && queue_dispatch(q, PortId::Work, 0));
  CHECK(!queue_dispatch(q, PortId::Work, 0));
  CHECK(log.order.size() == 3 && log.order[1] == 1 && log.order[2] == 2);
  Call d{&log, 4};
  CHECK(queue_submit(q, PortId::Work, 30, &d, &record) == S_OK);
  CHECK(!queue_dispatch(q, PortId::Work, 0));
  CHECK(queue_dispatch(q, PortId::Work, 2000) && log.order.back() == 4);
  queue_close(q);
}

TEST_CASE(task_queue_thread_pools) {
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  for (DispatchMode mode : {DispatchMode::ThreadPool, DispatchMode::SerializedThreadPool}) {
    g_count = 0;
    g_max_concurrent = 0;
    ResetEvent(done);
    TaskQueue* q = nullptr;
    CHECK(queue_create(mode, mode, &q) == S_OK);
    for (int i = 0; i < 20; ++i) CHECK(queue_submit(q, PortId::Work, 0, done, &count) == S_OK);
    CHECK(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
    if (mode == DispatchMode::SerializedThreadPool) CHECK(g_max_concurrent == 1);
    queue_close(q);
  }
  // The process queue stands in for a null queue.
  g_count = 0;
  ResetEvent(done);
  for (int i = 0; i < 20; ++i) CHECK(queue_submit(nullptr, PortId::Work, 0, done, &count) == S_OK);
  CHECK(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
  TaskQueue* process = nullptr;
  CHECK(process_queue_get(&process) && process);
  queue_close(process);
  CloseHandle(done);
}

TEST_CASE(task_queue_terminate_monitor_composite_waiter) {
  TaskQueue* q = nullptr;
  CHECK(queue_create(DispatchMode::Manual, DispatchMode::Manual, &q) == S_OK);
  uint64_t token = 0;
  g_monitored = 0;
  CHECK(queue_register_monitor(q, nullptr, &monitor, &token) == S_OK);
  Log log;
  Call a{&log, 1}, b{&log, 2};
  CHECK(queue_submit(q, PortId::Work, 0, &a, &record) == S_OK);
  CHECK(queue_submit(q, PortId::Work, 5000, &b, &record) == S_OK);  // delayed: cancelled too
  CHECK(g_monitored == 1);
  queue_unregister_monitor(q, token);

  // A composite queue over the first's ports: submitted through one, dispatched through either.
  TaskQueuePort* work = nullptr;
  TaskQueuePort* completion = nullptr;
  CHECK(queue_get_port(q, PortId::Work, &work) == S_OK && queue_get_port(q, PortId::Completion, &completion) == S_OK);
  TaskQueue* composite = nullptr;
  CHECK(queue_create_composite(work, completion, &composite) == S_OK);
  Call c{&log, 3};
  CHECK(queue_submit(composite, PortId::Completion, 0, &c, &record) == S_OK);
  CHECK(queue_dispatch(q, PortId::Completion, 0) && log.order.size() == 1 && log.order[0] == 3);

  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Call w{&log, 4};
  CHECK(queue_register_waiter(composite, PortId::Completion, event, &w, &record, &token) == S_OK);
  SetEvent(event);
  CHECK(queue_dispatch(q, PortId::Completion, 5000) && log.order.back() == 4);
  queue_unregister_waiter(composite, token);
  CloseHandle(event);

  // Termination cancels what is pending through that queue, then reports.
  bool ended = false;
  CHECK(queue_terminate(q, true, &ended, &terminated) == S_OK);
  CHECK(ended && log.canceled == 2 && log.order.size() == 4);
  CHECK(queue_submit(q, PortId::Work, 0, &a, &record) == E_ABORT);
  queue_close(composite);
  queue_close(q);
}

TEST_CASE(async_provider_protocol) {
  TaskQueue* q = nullptr;
  CHECK(queue_create(DispatchMode::Manual, DispatchMode::Manual, &q) == S_OK);
  AsyncBlock block{};
  block.queue = q;
  block.callback = &completed;
  g_completions = 0;
  static int identity;
  CHECK(async_begin(&block, nullptr, &identity, "test", &four_bytes) == S_OK);
  CHECK(async_get_status(&block, false) == kPending);
  CHECK(queue_dispatch(q, PortId::Work, 0));  // DoWork
  CHECK(g_completions == 0);                   // the routine goes through the completion port
  CHECK(queue_dispatch(q, PortId::Completion, 0) && g_completions == 1);
  CHECK(async_get_status(&block, true) == S_OK);
  size_t size = 0;
  CHECK(async_get_result_size(&block, &size) == S_OK && size == 4);
  char small[2];
  CHECK(async_get_result(&block, &identity, sizeof small, small, nullptr) == kNotSufficientBuffer);
  char result[4] = {};
  size_t used = 0;
  CHECK(async_get_result(&block, &identity, sizeof result, result, &used) == S_OK && used == 4);
  CHECK(std::memcmp(result, "done", 4) == 0);
  CHECK(async_get_status(&block, false) == S_OK);  // the block keeps its status
  queue_close(q);
}

TEST_CASE(async_begin_failure_cancel_and_run) {
  AsyncBlock a{};
  CHECK(async_begin(&a, nullptr, nullptr, nullptr, &fails_to_begin) == S_OK);  // the call fails, not Begin
  CHECK(async_get_status(&a, true) == E_FAIL);

  AsyncBlock b{};
  CHECK(async_begin(&b, nullptr, nullptr, nullptr, &does_nothing) == S_OK);
  async_cancel(&b);  // a provider that ignores Cancel goes on
  CHECK(async_get_status(&b, false) == kPending);
  async_complete(&b, kPending, 0);  // not a completion
  CHECK(async_get_status(&b, false) == kPending);
  async_complete(&b, S_OK, 0);
  CHECK(async_get_status(&b, true) == S_OK);

  AsyncBlock c{};
  CHECK(async_begin(&c, nullptr, nullptr, nullptr, &aborts_on_cancel) == S_OK);
  async_cancel(&c);
  CHECK(async_get_status(&c, true) == E_ABORT);

  AsyncBlock d{};
  CHECK(async_run(&d, &work_returns_7) == S_OK);
  CHECK(async_get_status(&d, true) == 7);
  CHECK(thread_set_time_sensitive(true) == S_OK && thread_is_time_sensitive());
  thread_set_time_sensitive(false);
}
