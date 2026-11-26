#pragma once

#include <filesystem>

namespace am::rt {

// The shader service (`am-shaders serve`) compiles the programs the game waits for while it
// runs. A game run starts it when the title creates its D3D12 device, unless one is running
// already from the same am-shaders.exe; one from an older build is asked to stop and replaced.
// It runs from a copy in <cache>\shaders\service (so builds can replace am-shaders.exe), below
// normal priority, outside the run's job when the host allows that, logs to <logs>\serve.txt,
// and keeps running after the game ends.
//
// `enabled` false (americas.ini `shader_service = off`, or --no-shader-service) never starts
// one; `jobs` 0 picks about half the host's threads.
void configure_shader_service(bool enabled, unsigned jobs, const std::filesystem::path& cache_dir,
                              const std::filesystem::path& logs_dir);
// Starts the service if configured and needed, on a thread of its own; only the first call acts.
void start_shader_service();

}  // namespace am::rt
