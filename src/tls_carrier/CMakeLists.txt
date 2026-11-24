# One static TLS slot each for title DLLs (see runtime/tls.h).
foreach(n RANGE 1 4)
  add_library(am_tls_${n} SHARED carrier.cpp)
  target_compile_definitions(am_tls_${n} PRIVATE AM_TLS_CARRIER_NAME=am_tls_carrier_${n})
  target_link_libraries(am_tls_${n} PRIVATE am_options)
endforeach()
