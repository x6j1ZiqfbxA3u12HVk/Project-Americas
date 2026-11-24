// The runtime's XAsync / XTaskQueue / XThread object. Its methods take the object first, then
// the public function's arguments.

#include "runtime/gdk/runtime.h"
#include "runtime/gdk/task_queue.h"

namespace am::rt::gdk {
namespace {

using Self = void*;

HRESULT WINAPI get_status(Self, AsyncBlock* b, bool wait) { return async_get_status(b, wait); }
HRESULT WINAPI get_result_size(Self, AsyncBlock* b, size_t* size) { return async_get_result_size(b, size); }
void WINAPI cancel(Self, AsyncBlock* b) { async_cancel(b); }
HRESULT WINAPI run(Self, AsyncBlock* b, AsyncWork work) { return async_run(b, work); }
HRESULT WINAPI begin(Self, AsyncBlock* b, void* context, const void* identity, const char* name, AsyncProvider p) {
  return async_begin(b, context, identity, name, p);
}
HRESULT WINAPI schedule(Self, AsyncBlock* b, uint32_t delay_ms) { return async_schedule(b, delay_ms); }
void WINAPI complete(Self, AsyncBlock* b, HRESULT result, size_t size) { async_complete(b, result, size); }
HRESULT WINAPI get_result(Self, AsyncBlock* b, const void* identity, size_t size, void* buffer, size_t* used) {
  return async_get_result(b, identity, size, buffer, used);
}

HRESULT WINAPI create(Self, DispatchMode work, DispatchMode completion, TaskQueue** out) {
  return queue_create(work, completion, out);
}
HRESULT WINAPI create_composite(Self, TaskQueuePort* work, TaskQueuePort* completion, TaskQueue** out) {
  return queue_create_composite(work, completion, out);
}
HRESULT WINAPI get_port(Self, TaskQueue* q, PortId port, TaskQueuePort** out) { return queue_get_port(q, port, out); }
HRESULT WINAPI duplicate(Self, TaskQueue* q, TaskQueue** out) { return queue_duplicate(q, out); }
bool WINAPI dispatch(Self, TaskQueue* q, PortId port, uint32_t timeout_ms) { return queue_dispatch(q, port, timeout_ms); }
void WINAPI close(Self, TaskQueue* q) { queue_close(q); }
HRESULT WINAPI submit(Self, TaskQueue* q, PortId port, void* context, TaskCallback callback) {
  return queue_submit(q, port, 0, context, callback);
}
HRESULT WINAPI submit_delayed(Self, TaskQueue* q, PortId port, uint32_t delay_ms, void* context, TaskCallback callback) {
  return queue_submit(q, port, delay_ms, context, callback);
}
HRESULT WINAPI register_waiter(Self, TaskQueue* q, PortId port, HANDLE handle, void* context, TaskCallback callback,
                               uint64_t* token) {
  return queue_register_waiter(q, port, handle, context, callback, token);
}
void WINAPI unregister_waiter(Self, TaskQueue* q, uint64_t token) { queue_unregister_waiter(q, token); }
HRESULT WINAPI terminate(Self, TaskQueue* q, bool wait, void* context, TerminatedCallback callback) {
  return queue_terminate(q, wait, context, callback);
}
HRESULT WINAPI register_monitor(Self, TaskQueue* q, void* context, MonitorCallback callback, uint64_t* token) {
  return queue_register_monitor(q, context, callback, token);
}
void WINAPI unregister_monitor(Self, TaskQueue* q, uint64_t token) { queue_unregister_monitor(q, token); }
bool WINAPI get_process_queue(Self, TaskQueue** out) { return process_queue_get(out); }
void WINAPI set_process_queue(Self, TaskQueue* q) { process_queue_set(q); }

HRESULT WINAPI set_time_sensitive(Self, bool sensitive) { return thread_set_time_sensitive(sensitive); }
void WINAPI assert_not_time_sensitive(Self) {}  // a debug check on the console
bool WINAPI is_time_sensitive(Self) { return thread_is_time_sensitive(); }

}  // namespace

void add_task_queue_interfaces(std::vector<Interface>& out) {
  const GUID id = guid("073b7dcb-1fcf-4030-94be-e3c9eb623428");
  out.push_back({"XAsync/XTaskQueue",
                 id,
                 id,
                 {
                     {3, "XAsyncGetStatus", AM_GDK(get_status)},
                     {4, "XAsyncGetResultSize", AM_GDK(get_result_size)},
                     {5, "XAsyncCancel", AM_GDK(cancel), Returns::Zero},
                     {6, "XAsyncRun", AM_GDK(run)},
                     {7, "XAsyncBegin", AM_GDK(begin)},
                     {9, "XAsyncSchedule", AM_GDK(schedule)},
                     {10, "XAsyncComplete", AM_GDK(complete), Returns::Zero},
                     {11, "XAsyncGetResult", AM_GDK(get_result)},
                     {12, "XTaskQueueCreate", AM_GDK(create)},
                     {13, "XTaskQueueCreateComposite", AM_GDK(create_composite)},
                     {14, "XTaskQueueGetPort", AM_GDK(get_port)},
                     {15, "XTaskQueueDuplicateHandle", AM_GDK(duplicate)},
                     {16, "XTaskQueueDispatch", AM_GDK(dispatch), Returns::Zero},
                     {17, "XTaskQueueCloseHandle", AM_GDK(close), Returns::Zero},
                     {18, "XTaskQueueSubmitCallback", AM_GDK(submit)},
                     {19, "XTaskQueueSubmitDelayedCallback", AM_GDK(submit_delayed)},
                     {20, "XTaskQueueRegisterWaiter", AM_GDK(register_waiter)},
                     {21, "XTaskQueueUnregisterWaiter", AM_GDK(unregister_waiter), Returns::Zero},
                     {22, "XTaskQueueTerminate", AM_GDK(terminate)},
                     {23, "XTaskQueueRegisterMonitor", AM_GDK(register_monitor)},
                     {24, "XTaskQueueUnregisterMonitor", AM_GDK(unregister_monitor), Returns::Zero},
                     {25, "XTaskQueueGetCurrentProcessTaskQueue", AM_GDK(get_process_queue), Returns::Zero},
                     {26, "XTaskQueueSetCurrentProcessTaskQueue", AM_GDK(set_process_queue), Returns::Zero},
                     {27, "XThreadSetTimeSensitive", AM_GDK(set_time_sensitive)},
                     {28, "XThreadVerifyNotTimeSensitive", nullptr},
                     {29, "XThreadAssertNotTimeSensitive", AM_GDK(assert_not_time_sensitive), Returns::Zero},
                     {30, "XThreadIsTimeSensitive", AM_GDK(is_time_sensitive), Returns::Zero},
                 }});
}

}  // namespace am::rt::gdk
