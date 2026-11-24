#pragma once

// Static TLS for title modules.
//
// Windows gives static TLS only to modules its own loader loaded. So our launcher and a few
// "carrier" DLLs each reserve a large static TLS block, and every title module that has a TLS
// directory gets one carrier's slot:
//  * the carrier's template is overwritten with the title module's template, so every thread
//    created from now on starts with the title's initial values;
//  * the blocks of threads that already exist are filled the same way;
//  * the title module's TLS index variable is set to the carrier's slot.
// The title executable gets the launcher's slot 0, which is what executable code expects.

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/image.h"

namespace am::rt {

struct TlsCarrier {
  std::string module;      // "americas.exe", "am_tls_1.dll", ...
  uint32_t index = 0;      // its TLS slot
  uint8_t* tmpl = nullptr; // its template in its image
  size_t capacity = 0;     // template + zero fill
  bool used = false;
};

class TlsManager {
 public:
  // Finds the carriers: the launcher (slot 0) and the loaded am_tls_N.dll modules.
  bool discover(std::string& error);
  // Gives `image` a carrier slot (the executable: slot 0). No-op without a TLS directory.
  bool install(MappedImage& image, bool is_executable, std::string& error);
  const std::vector<TlsCarrier>& carriers() const { return carriers_; }

 private:
  std::vector<TlsCarrier> carriers_;
};

}  // namespace am::rt
