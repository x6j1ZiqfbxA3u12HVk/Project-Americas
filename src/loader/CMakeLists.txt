add_library(am_loader STATIC import_policy.cpp)
target_link_libraries(am_loader PUBLIC am_core)
