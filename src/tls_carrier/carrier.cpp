// am_tls_<N>.dll: one static TLS slot for one title DLL (see runtime/tls.h). The runtime
// overwrites this module's TLS template with the title DLL's and points the title DLL's TLS
// index at this module's slot.

#include <windows.h>

#ifndef AM_TLS_CARRIER_NAME
#error "AM_TLS_CARRIER_NAME must name the export"
#endif

namespace {
__declspec(thread) alignas(64) unsigned char g_block[16 * 1024];
}

// The runtime imports this so the Windows loader loads the carrier, with its slot, at start-up.
extern "C" __declspec(dllexport) void* AM_TLS_CARRIER_NAME() { return &g_block[0]; }

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
