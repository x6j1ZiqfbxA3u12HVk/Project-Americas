#pragma once

// Host pipelines kept between runs (pipeline_cache.cpp): loaded from a D3D12 pipeline library
// when an earlier run compiled them, else compiled by the driver (two at a time) and stored.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace am::rt::d3d12x {

// A compute pipeline for `desc` (a reference of its own: the caller releases it), or nullptr.
ID3D12PipelineState* cached_compute_pipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc);
// The same for a graphics pipeline; `result` gets the driver's answer when it compiles.
ID3D12PipelineState* cached_graphics_pipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, HRESULT* result = nullptr);
// Writes the library to disk if pipelines were stored since the last save.
void save_pipeline_cache();
// "pipelines loaded from the library N, compiled M", for the frame summary.
std::string pipeline_cache_counts();

}  // namespace am::rt::d3d12x
