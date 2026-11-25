#pragma once

// Command lists record what the title asks for, so a back end can run it.
//
// The console's command list has the same methods as D3D12's ID3D12GraphicsCommandList, in
// D3D12's order up to SetPipelineState (slot 25), then four slots later from SetDescriptorHeaps
// (D3D12's 28, the console's 32) to ExecuteIndirect; Close is 9, Reset 139, the barrier call
// 146 (learned from the game's calls and their call sites).
//
// The title's code records most commands inline: it writes packets into a buffer in the list
// object (cursor at +0x18, end at +0x30) and calls the method only when the buffer is full. Our
// list objects keep both fields 0, so every command reaches its method.

#include <cstdint>
#include <vector>

namespace am::rt::d3d12x {

struct Object;

// Console slot numbers of the commands we name.
enum Op : uint16_t {
  kClose = 9,
  kDrawInstanced = 12,
  kDrawIndexedInstanced = 13,
  kDispatch = 14,
  kCopyBufferRegion = 15,
  kCopyTextureRegion = 16,
  kCopyResource = 17,
  kCopyTiles = 18,
  kResolveSubresource = 19,
  kSetPrimitiveTopology = 20,
  kSetViewports = 21,
  kSetScissorRects = 22,
  kSetBlendFactor = 23,
  kSetStencilRef = 24,
  kSetPipelineState = 25,
  kSetDescriptorHeaps = 32,
  kSetComputeRootSignature = 33,
  kSetGraphicsRootSignature = 34,
  kSetComputeRootDescriptorTable = 35,
  kSetGraphicsRootDescriptorTable = 36,
  kSetComputeRoot32BitConstant = 37,
  kSetGraphicsRoot32BitConstant = 38,
  kSetComputeRoot32BitConstants = 39,
  kSetGraphicsRoot32BitConstants = 40,
  kSetComputeRootCbv = 41,
  kSetGraphicsRootCbv = 42,
  kSetComputeRootSrv = 43,
  kSetGraphicsRootSrv = 44,
  kSetComputeRootUav = 45,
  kSetGraphicsRootUav = 46,
  kSetIndexBuffer = 47,
  kSetVertexBuffers = 48,
  kSetStreamOutputTargets = 49,
  kSetRenderTargets = 50,
  kClearDepthStencilView = 51,
  kClearRenderTargetView = 52,
  kClearUavUint = 53,
  kClearUavFloat = 54,
  kDiscardResource = 55,
  kBeginQuery = 56,
  kEndQuery = 57,
  kResolveQueryData = 58,
  kSetPredication = 59,
  kSetMarker = 60,
  kBeginEvent = 61,
  kEndEvent = 62,
  kExecuteIndirect = 63,
  kReset = 139,
  kBarrier = 146,
};

const char* op_name(uint16_t op);

// One recorded command: its arguments as passed (integer registers and stack slots), and the
// bytes its pointer arguments pointed at, copied while the call ran.
struct Command {
  uint16_t op = 0;
  uint32_t data_at = 0, data_size = 0;  // in the list's data
  uint64_t a[7] = {};
  float f[2] = {};  // float arguments (ClearDepthStencilView's depth)
};

// The commands a list holds since its last Reset, for a back end; nullptr if `list` is not one.
struct Recording {
  std::vector<Command> commands;
  std::vector<uint8_t> data;
  bool closed = false;
};
const Recording* recording_of(const void* list);

// queue.cpp's ExecuteCommandListsX: submits the lists' host command lists, in order.
void submit_lists(unsigned count, void* const* lists);

// queue.cpp's ExecuteCommandLists hands each list here; PresentX ends the frame.
void frame_execute(const void* list);
void frame_present();

}  // namespace am::rt::d3d12x
