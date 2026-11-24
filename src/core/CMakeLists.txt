add_library(am_core STATIC
  config.cpp
  file_view.cpp
  json.cpp
  paths.cpp
  sha256.cpp
  text.cpp)
target_include_directories(am_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(am_core PUBLIC am_options PRIVATE bcrypt)
