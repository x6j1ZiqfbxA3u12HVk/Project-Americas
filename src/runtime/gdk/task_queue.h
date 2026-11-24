#pragma once

// XTaskQueue and XAsync: the GDK's work queues and asynchronous calls (plan, section 5.4). The
// other runtime areas build their asynchronous calls on these, so they are complete:
//
// * a queue has a work port and a completion port, each with its dispatch mode: Manual (the
//   title calls XTaskQueueDispatch), ThreadPool, SerializedThreadPool (one callback at a time,
//   in order) or Immediate (on the submitting thread);
// * delayed callbacks, waiters on handles, monitors told of each submission, termination that
//   cancels what is pending, composite queues over other queues' ports, and the process's
//   default queue, used wherever a queue is null;
// * an XAsyncBlock's call runs through its provider (Begin, DoWork, GetResult, Cancel, Cleanup)
//   on the block's queue: work on the work port, the completion routine on the completion port.
//
// The layouts and values are the public GDK API's.

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace am::rt::gdk {

enum class DispatchMode : uint32_t { Manual, ThreadPool, SerializedThreadPool, Immediate };
enum class PortId : uint32_t { Work, Completion };

struct TaskQueue;      // XTaskQueueHandle
struct TaskQueuePort;  // XTaskQueuePortHandle

using TaskCallback = void(CALLBACK*)(void* context, bool canceled);
using TerminatedCallback = void(CALLBACK*)(void* context);
using MonitorCallback = void(CALLBACK*)(void* context, TaskQueue* queue, PortId port);

constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000A);  // E_PENDING
constexpr HRESULT kNotSufficientBuffer = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

HRESULT queue_create(DispatchMode work, DispatchMode completion, TaskQueue** out);
HRESULT queue_create_composite(TaskQueuePort* work, TaskQueuePort* completion, TaskQueue** out);
HRESULT queue_get_port(TaskQueue* queue, PortId port, TaskQueuePort** out);
HRESULT queue_duplicate(TaskQueue* queue, TaskQueue** out);
void queue_close(TaskQueue* queue);
// Runs one callback waiting on a Manual port; true if one ran. 0xFFFFFFFF waits forever.
bool queue_dispatch(TaskQueue* queue, PortId port, uint32_t timeout_ms);
HRESULT queue_submit(TaskQueue* queue, PortId port, uint32_t delay_ms, void* context, TaskCallback callback);
HRESULT queue_register_waiter(TaskQueue* queue, PortId port, HANDLE handle, void* context, TaskCallback callback,
                              uint64_t* token);
void queue_unregister_waiter(TaskQueue* queue, uint64_t token);
HRESULT queue_terminate(TaskQueue* queue, bool wait, void* context, TerminatedCallback callback);
HRESULT queue_register_monitor(TaskQueue* queue, void* context, MonitorCallback callback, uint64_t* token);
void queue_unregister_monitor(TaskQueue* queue, uint64_t token);
// The process's queue (thread pool on both ports, made on first use), duplicated for the caller.
bool process_queue_get(TaskQueue** out);
void process_queue_set(TaskQueue* queue);

// XThreadSetTimeSensitive and friends: a per-thread flag.
HRESULT thread_set_time_sensitive(bool sensitive);
bool thread_is_time_sensitive();

// ---- XAsync ----

struct AsyncBlock;
using AsyncCompletion = void(CALLBACK*)(AsyncBlock* block);
struct AsyncBlock {
  TaskQueue* queue;  // null: the process queue
  void* context;
  AsyncCompletion callback;
  unsigned char internal[sizeof(void*) * 4];
};
static_assert(sizeof(AsyncBlock) == 0x38);

enum class AsyncOp : uint32_t { Begin, DoWork, GetResult, Cancel, Cleanup };
struct AsyncProviderData {
  AsyncBlock* async;
  size_t buffer_size;
  void* buffer;
  void* context;
};
using AsyncProvider = HRESULT(CALLBACK*)(AsyncOp op, const AsyncProviderData* data);
using AsyncWork = HRESULT(CALLBACK*)(AsyncBlock* block);

HRESULT async_begin(AsyncBlock* block, void* context, const void* identity, const char* name, AsyncProvider provider);
HRESULT async_schedule(AsyncBlock* block, uint32_t delay_ms);
void async_complete(AsyncBlock* block, HRESULT result, size_t required_size);
HRESULT async_get_status(AsyncBlock* block, bool wait);
HRESULT async_get_result_size(AsyncBlock* block, size_t* size);
HRESULT async_get_result(AsyncBlock* block, const void* identity, size_t size, void* buffer, size_t* used);
void async_cancel(AsyncBlock* block);
HRESULT async_run(AsyncBlock* block, AsyncWork work);

}  // namespace am::rt::gdk
