cmake_minimum_required(VERSION 3.28)
project(ProjectAmericas VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "Project Americas builds with 64-bit MSVC only. Enter the x64 developer environment first: . .\\scripts\\env.ps1")
endif()

# Generated files never land in the repository: the build tree has to live outside it.
cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${PROJECT_BINARY_DIR}" NORMALIZE build_inside_source)
if(build_inside_source)
  message(FATAL_ERROR "Configure with a preset (cmake --preset release): build trees belong under C:/Project Americas/build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# Debug information inside the objects (/Z7): parallel Ninja builds never fight over one PDB.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# Options every target of ours builds with.
add_library(am_options INTERFACE)
target_compile_options(am_options INTERFACE
  /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /EHsc /diagnostics:caret)
target_compile_definitions(am_options INTERFACE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  # Windows 10 2004: the floor that declares placeholders (VirtualAlloc2, MapViewOfFile3).
  NTDDI_VERSION=0x0A000008)
target_link_options(am_options INTERFACE /DEBUG $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

add_subdirectory(src/core)
add_subdirectory(src/pe)
add_subdirectory(src/host)
add_subdirectory(src/x86)
add_subdirectory(src/formats)
add_subdirectory(src/shader)
add_subdirectory(src/loader)
add_subdirectory(src/tls_carrier)
add_subdirectory(src/runtime)
add_subdirectory(src/launcher)
add_subdirectory(src/tools/am-probe)
add_subdirectory(src/tools/am-inspect)
add_subdirectory(src/tools/am-cpuscan)
add_subdirectory(src/tools/am-shaders)

enable_testing()
add_subdirectory(tests)
