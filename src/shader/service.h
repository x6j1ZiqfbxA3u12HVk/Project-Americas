#pragma once

// What the shader service (`am-shaders serve`) and the runtime that starts it agree on.

namespace am::shader {

// Held by the running service: one service at a time.
inline constexpr wchar_t kServiceMutex[] = L"Local\\AmericasShaderService";
// Set to ask the running service to stop (the runtime does, when a newer am-shaders.exe exists).
inline constexpr wchar_t kServiceStopEvent[] = L"Local\\AmericasShaderServiceStop";
// <cache>\shaders\service\running.txt: the full path of the executable the service runs from,
// written by the service when it starts.
inline constexpr wchar_t kServiceFolder[] = L"service";
inline constexpr wchar_t kServiceRunningFile[] = L"running.txt";

}  // namespace am::shader
