// The runtime areas of an offline retail console with one signed-in local user (plan, section
// 5.4): XUser, XSystem, XGame, XNetworking (offline), the feature check, game events, the
// installed package (XPackage), local storage, and HDR (off).
// Methods take the object first, then the public function's arguments. Structures and values
// are the public GDK API's.

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string_view>

#include "core/text.h"
#include "runtime/console.h"
#include "runtime/gdk/runtime.h"
#include "runtime/gdk/task_queue.h"
#include "runtime/log.h"
#include "runtime/title_config.h"

namespace am::rt::gdk {
namespace {

using Self = void*;

constexpr HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kOffline = HRESULT_FROM_WIN32(ERROR_NO_NETWORK);  // what online-only calls fail with

// Copies `text` and its NUL into the caller's buffer, as the runtime's string getters do.
HRESULT put_string(std::string_view text, size_t size, char* buffer, size_t* used) {
  if (!buffer || size < text.size() + 1) return kNotSufficientBuffer;
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = 0;
  if (used) *used = text.size() + 1;
  return S_OK;
}

std::atomic<uint64_t> g_next_token{1};
HRESULT new_token(uint64_t* token) {
  if (!token) return E_POINTER;
  *token = g_next_token.fetch_add(1);
  return S_OK;
}

// An asynchronous call that completes at once with `result` (no result data). The context is
// the HRESULT.
HRESULT CALLBACK completes_with(AsyncOp op, const AsyncProviderData* data) {
  if (op == AsyncOp::Begin) {
    async_complete(data->async, static_cast<HRESULT>(reinterpret_cast<intptr_t>(data->context)), 0);
  }
  return S_OK;
}
HRESULT begin_completed(AsyncBlock* block, const void* identity, HRESULT result) {
  return async_begin(block, reinterpret_cast<void*>(static_cast<intptr_t>(result)), identity, nullptr, &completes_with);
}

// ---- XUser: one local user, signed in from the start ----

constexpr uint32_t kUserMagic = 0x52535541;  // "AUSR"
struct User {
  uint32_t magic = kUserMagic;
  std::atomic<long> refs{1};
  uint64_t local_id = 1;
  uint64_t xuid = 0x0009000000A3E1C0;  // ours; an offline user has no Xbox network identity
};
User g_user;
const char kGamertag[] = "Player";

bool is_user(const User* u) { return u == &g_user; }
User* duplicate_user() {
  g_user.refs.fetch_add(1, std::memory_order_relaxed);
  return &g_user;
}

HRESULT WINAPI user_duplicate(Self, User* u, User** out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = duplicate_user();
  return S_OK;
}
void WINAPI user_close(Self, User* u) {
  if (is_user(u)) u->refs.fetch_sub(1, std::memory_order_relaxed);  // the user outlives its handles
}
int32_t WINAPI user_compare(Self, User* a, User* b) { return a == b ? 0 : a < b ? -1 : 1; }

int g_add_identity;
// XUserAddAsync: the default user, whatever the options (silently, with UI, guests allowed).
HRESULT CALLBACK add_provider(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork:
      async_complete(data->async, S_OK, sizeof(User*));
      return S_OK;
    case AsyncOp::GetResult:
      *static_cast<User**>(data->buffer) = duplicate_user();
      return S_OK;
    default:
      return S_OK;
  }
}
HRESULT WINAPI user_add_async(Self, uint32_t options, AsyncBlock* block) {
  logf("XUserAddAsync(options %u): the local user", options);
  return async_begin(block, nullptr, &g_add_identity, "XUserAddAsync", &add_provider);
}
HRESULT WINAPI user_add_result(Self, AsyncBlock* block, User** out) {
  if (!out) return E_POINTER;
  return async_get_result(block, &g_add_identity, sizeof(User*), out, nullptr);
}
HRESULT WINAPI user_get_local_id(Self, User* u, uint64_t* out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = u->local_id;
  return S_OK;
}
HRESULT WINAPI user_find_by_local_id(Self, uint64_t id, User** out) {
  if (!out) return E_POINTER;
  if (id != g_user.local_id) return kNotFound;
  *out = duplicate_user();
  return S_OK;
}
HRESULT WINAPI user_get_id(Self, User* u, uint64_t* out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = u->xuid;
  return S_OK;
}
HRESULT WINAPI user_find_by_id(Self, uint64_t xuid, User** out) {
  if (!out) return E_POINTER;
  if (xuid != g_user.xuid) return kNotFound;
  *out = duplicate_user();
  return S_OK;
}
HRESULT WINAPI user_get_is_guest(Self, User* u, bool* out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = false;
  return S_OK;
}
HRESULT WINAPI user_get_state(Self, User* u, uint32_t* out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = 0;  // XUserState::SignedIn
  return S_OK;
}
HRESULT WINAPI user_get_age_group(Self, User* u, uint32_t* out) {
  if (!is_user(u) || !out) return E_INVALIDARG;
  *out = 3;  // XUserAgeGroup::Adult
  return S_OK;
}
// Offline, the privileges that need the Xbox network (multiplayer, communication, user
// content) are not held.
HRESULT WINAPI user_check_privilege(Self, User* u, uint32_t /*options*/, uint32_t privilege, bool* has,
                                    uint32_t* reason) {
  if (!is_user(u) || !has || !reason) return E_INVALIDARG;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) logf("XUserCheckPrivilege(%u): not held offline", privilege);
  *has = false;
  *reason = 2;  // XUserPrivilegeDenyReason::Restricted
  return S_OK;
}
int g_resolve_identity;
HRESULT WINAPI user_resolve_privilege_async(Self, User* u, uint32_t, uint32_t, AsyncBlock* block) {
  if (!is_user(u)) return E_INVALIDARG;
  return begin_completed(block, &g_resolve_identity, E_ABORT);  // as if the user dismissed it
}
HRESULT WINAPI user_resolve_privilege_result(Self, AsyncBlock* block) {
  return async_get_result(block, &g_resolve_identity, 0, nullptr, nullptr);
}
int g_token_identity;
HRESULT WINAPI user_get_token_async(Self, User* u, uint32_t, const char*, const char*, size_t, const void*, size_t,
                                    const void*, AsyncBlock* block) {
  if (!is_user(u)) return E_INVALIDARG;
  return begin_completed(block, &g_token_identity, kOffline);
}
HRESULT WINAPI user_get_token_result_size(Self, AsyncBlock* block, size_t* size) {
  return async_get_result_size(block, size);
}
HRESULT WINAPI user_get_token_result(Self, AsyncBlock* block, size_t size, void* buffer, void** pointer, size_t* used) {
  if (pointer) *pointer = nullptr;
  return async_get_result(block, &g_token_identity, size, buffer, used);
}
HRESULT WINAPI user_register_for_change(Self, TaskQueue*, void*, void*, uint64_t* token) { return new_token(token); }
bool WINAPI user_unregister_for_change(Self, uint64_t, bool) { return true; }
HRESULT WINAPI user_get_gamertag(Self, User* u, uint32_t /*component*/, size_t size, char* buffer, size_t* used) {
  if (!is_user(u)) return E_INVALIDARG;
  return put_string(kGamertag, size, buffer, used);
}

// ---- XSystem ----

// Ids of this installation: made up, stable, and the same for every run.
HRESULT WINAPI system_console_id(Self, size_t size, char* buffer, size_t* used) {
  return put_string("AMERICAS.OFFLINE.CONSOLE.0001", size, buffer, used);
}
HRESULT WINAPI system_sandbox_id(Self, size_t size, char* buffer, size_t* used) {
  return put_string("RETAIL", size, buffer, used);
}
HRESULT WINAPI system_app_device_id(Self, size_t size, char* buffer, size_t* used) {
  return put_string("AMERICAS.OFFLINE.DEVICE.0001", size, buffer, used);
}

struct Version {
  uint16_t major, minor, build, revision;
};
struct AnalyticsInfo {
  Version os_version;
  Version hosting_os_version;
  char family[64];
  char form[64];
};
static_assert(sizeof(AnalyticsInfo) == 0x90);

// Returned through a hidden pointer, as the game's thunk expects (it copies 0x90 bytes back).
AnalyticsInfo* WINAPI system_analytics(Self, AnalyticsInfo* out) {
  std::memset(out, 0, sizeof *out);
  out->os_version = {10, 0, 25398, 0};
  out->hosting_os_version = {10, 0, 25398, 0};
  strcpy_s(out->family, "Windows.Xbox");
  strcpy_s(out->form, current_console().console == Console::SeriesX ? "Xbox Series X" : "Xbox Series S");
  return out;
}

// ---- XGame ----

HRESULT WINAPI game_title_id(Self, uint32_t* out) {
  if (!out) return E_POINTER;
  *out = title_config().title_id;
  return S_OK;
}
void WINAPI game_launch_new(Self, const char* path, const char* args, User*) {
  logf("XLaunchNewGame(%s, %s) ignored", path ? path : "", args ? args : "");
}

// ---- XNetworking, offline ----

constexpr uint16_t kUdpPort = 3074;  // the console's usual multiplayer port

HRESULT WINAPI net_udp_port(Self, uint16_t* port) {
  if (!port) return E_POINTER;
  *port = kUdpPort;
  return S_OK;
}
int g_port_identity;
HRESULT CALLBACK port_provider(AsyncOp op, const AsyncProviderData* data) {
  if (op == AsyncOp::Begin) async_complete(data->async, S_OK, sizeof(uint16_t));
  if (op == AsyncOp::GetResult) *static_cast<uint16_t*>(data->buffer) = kUdpPort;
  return S_OK;
}
HRESULT WINAPI net_udp_port_async(Self, AsyncBlock* block) {
  return async_begin(block, nullptr, &g_port_identity, nullptr, &port_provider);
}
HRESULT WINAPI net_udp_port_result(Self, AsyncBlock* block, uint16_t* port) {
  return async_get_result(block, &g_port_identity, sizeof(uint16_t), port, nullptr);
}
int g_security_identity;
HRESULT WINAPI net_security_async(Self, const char*, AsyncBlock* block) {
  return begin_completed(block, &g_security_identity, kOffline);
}
HRESULT WINAPI net_security_size(Self, AsyncBlock* block, size_t* size) { return async_get_result_size(block, size); }
HRESULT WINAPI net_security_result(Self, AsyncBlock* block, size_t size, void* buffer, void** pointer, size_t* used) {
  if (pointer) *pointer = nullptr;
  return async_get_result(block, &g_security_identity, size, buffer, used);
}
HRESULT WINAPI net_verify_certificate(Self, void*, const void*) { return kOffline; }

struct ConnectivityHint {
  uint32_t level;  // XNetworkingConnectivityLevelHint
  uint32_t cost;   // XNetworkingConnectivityCostHint
  uint32_t iana_interface_type;
  bool network_initialized;
  bool approaching_data_limit;
  bool over_data_limit;
  bool roaming;
};
static_assert(sizeof(ConnectivityHint) == 16);

HRESULT WINAPI net_connectivity(Self, ConnectivityHint* out) {
  if (!out) return E_POINTER;
  *out = ConnectivityHint{};
  out->level = 1;  // XNetworkingConnectivityLevelHint::None: the network is off
  out->network_initialized = true;
  return S_OK;
}
HRESULT WINAPI net_register_hint(Self, TaskQueue*, void*, void*, uint64_t* token) { return new_token(token); }
bool WINAPI net_unregister_hint(Self, uint64_t, bool) { return true; }

// ---- the rest ----

// A console has every runtime feature (the game asks about XNetworking and XGameEvent).
bool WINAPI feature_available(Self, uint32_t feature) { return feature <= 21; }

HRESULT WINAPI game_event_write(Self, User*, const char*, const char*, const char* name, const char*, const char*) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) logf("XGameEventWrite(%s): dropped, offline", name ? name : "");
  return S_OK;
}

// ---- XPackage: this process is the one package, fully installed ----
// An installed retail package: every chunk on disk, nothing to download, launchable.
struct InstallationProgress {
  uint64_t total_bytes, installed_bytes, launch_bytes;
  bool launchable, completed;
};
uint64_t g_monitor = 0x50ACA6E;  // the one installation monitor handle we give out

bool WINAPI package_is_packaged(Self) { return true; }
HRESULT WINAPI package_identifier(Self, size_t size, char* buffer) {
  const TitleConfig& t = title_config();
  return put_string(t.identity_name.empty() ? "Americas" : t.identity_name, size, buffer, nullptr);
}
HRESULT WINAPI package_create_monitor(Self, const char*, uint32_t, const void*, uint32_t, void*, void** out) {
  if (!out) return E_INVALIDARG;
  *out = &g_monitor;
  return S_OK;
}
void WINAPI package_close_monitor(Self, void*) {}
void WINAPI package_progress(Self, void*, InstallationProgress* p) {
  if (!p) return;
  const uint64_t size = 1ull << 30;
  *p = {size, size, size, true, true};
}
// (monitor, context, callback, token): installation is complete, so the callback never runs.
HRESULT WINAPI package_register_progress(Self, void*, void*, void*, uint64_t* token) { return new_token(token); }
HRESULT WINAPI package_chunk_availability(Self, const char*, uint32_t, const void*, uint32_t* availability) {
  if (!availability) return E_INVALIDARG;
  *availability = 0;  // XPackageChunkAvailability::Ready
  return S_OK;
}
// (kind, scope, context, callback): no other packages (DLC, content) are installed, so the
// callback is never called.
HRESULT WINAPI package_enumerate(Self, uint32_t, uint32_t, void*, void*) { return S_OK; }
HRESULT WINAPI package_estimate_download(Self, const char*, uint32_t, const void*, uint64_t* size, bool* confirm) {
  if (size) *size = 0;
  if (confirm) *confirm = false;
  return S_OK;
}

// ---- XPersistentLocalStorage: S:\ (console_paths maps it into userdata) ----
struct LocalStorageSpace {
  uint64_t available_free, total_free, used, total;
};
HRESULT WINAPI local_storage_path(Self, size_t size, char* buffer, size_t* used) { return put_string("S:\\", size, buffer, used); }
HRESULT WINAPI local_storage_space(Self, LocalStorageSpace* out) {
  if (!out) return E_INVALIDARG;
  const uint64_t total = 4ull << 30;
  *out = {total, total, 0, total};
  return S_OK;
}

// ---- XDisplay: HDR stays off (the presenter shows SDR) ----
uint32_t WINAPI display_try_enable_hdr(Self, uint32_t, void* info) {
  if (info) std::memset(info, 0, 16);
  return 2;  // XDisplayHdrModeResult::Disabled
}

}  // namespace

void add_offline_interfaces(std::vector<Interface>& out) {
  constexpr auto Z = Returns::Zero;
  const GUID user = guid("01acd177-91f9-4763-a38e-ccbb55ce32e0");
  out.push_back({"XUser", user, user, {
      {3, "XUserDuplicateHandle", AM_GDK(user_duplicate)},
      {4, "XUserCloseHandle", AM_GDK(user_close), Z},
      {5, "XUserCompare", AM_GDK(user_compare), Z},
      {7, "XUserAddAsync", AM_GDK(user_add_async)},
      {8, "XUserAddResult", AM_GDK(user_add_result)},
      {9, "XUserGetLocalId", AM_GDK(user_get_local_id)},
      {10, "XUserFindUserByLocalId", AM_GDK(user_find_by_local_id)},
      {11, "XUserGetId", AM_GDK(user_get_id)},
      {12, "XUserFindUserById", AM_GDK(user_find_by_id)},
      {13, "XUserGetIsGuest", AM_GDK(user_get_is_guest)},
      {14, "XUserGetState", AM_GDK(user_get_state)},
      {19, "XUserGetAgeGroup", AM_GDK(user_get_age_group)},
      {20, "XUserCheckPrivilege", AM_GDK(user_check_privilege)},
      {21, "XUserResolvePrivilegeWithUiAsync", AM_GDK(user_resolve_privilege_async)},
      {22, "XUserResolvePrivilegeWithUiResult", AM_GDK(user_resolve_privilege_result)},
      {23, "XUserGetTokenAndSignatureAsync", AM_GDK(user_get_token_async)},
      {24, "XUserGetTokenAndSignatureResultSize", AM_GDK(user_get_token_result_size)},
      {25, "XUserGetTokenAndSignatureResult", AM_GDK(user_get_token_result)},
      {33, "XUserRegisterForChangeEvent", AM_GDK(user_register_for_change)},
      {34, "XUserUnregisterForChangeEvent", AM_GDK(user_unregister_for_change), Z},
  }});
  out.push_back({"XUser (gamertag)", user, guid("cef4fac0-7676-4a94-a119-4c43f9eb5b74"), {
      {3, "XUserGetGamertag", AM_GDK(user_get_gamertag)},
  }});
  out.push_back({"XSystem", guid("e349bd1a-fc20-4e40-b99c-4178cc6b409f"), guid("1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf"), {
      {3, "XSystemGetConsoleId", AM_GDK(system_console_id)},
      {4, "XSystemGetXboxLiveSandboxId", AM_GDK(system_sandbox_id)},
      {5, "XSystemGetAppSpecificDeviceId", AM_GDK(system_app_device_id)},
  }});
  const GUID analytics = guid("b884675d-b738-4a9c-815d-9a9a1e0c6c9b");
  out.push_back({"XSystem (analytics)", analytics, analytics, {
      {3, "XSystemGetAnalyticsInfo", AM_GDK(system_analytics), Z},
  }});
  out.push_back({"XGame", guid("973a344e-24bf-4d0f-8457-56c534892b29"), guid("2549f142-6419-4a06-97b5-931aab7c2f34"), {
      {3, "XGameGetXboxTitleId", AM_GDK(game_title_id)},
      {4, "XLaunchNewGame", AM_GDK(game_launch_new), Z},
  }});
  out.push_back({"XNetworking", guid("37e56907-2f10-41e8-b72f-36edb185331a"),
                 guid("bf2346b2-39af-4658-b5ea-44713c7e83b3"), {
      {3, "XNetworkingQueryPreferredLocalUdpMultiplayerPort", AM_GDK(net_udp_port)},
      {4, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", AM_GDK(net_udp_port_async)},
      {5, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", AM_GDK(net_udp_port_result)},
      {8, "XNetworkingQuerySecurityInformationForUrlAsync", AM_GDK(net_security_async)},
      {9, "XNetworkingQuerySecurityInformationForUrlAsyncResultSize", AM_GDK(net_security_size)},
      {10, "XNetworkingQuerySecurityInformationForUrlAsyncResult", AM_GDK(net_security_result)},
      {14, "XNetworkingVerifyServerCertificate", AM_GDK(net_verify_certificate)},
      {15, "XNetworkingGetConnectivityHint", AM_GDK(net_connectivity)},
      {16, "XNetworkingRegisterConnectivityHintChanged", AM_GDK(net_register_hint)},
      {17, "XNetworkingUnregisterConnectivityHintChanged", AM_GDK(net_unregister_hint), Z},
  }});
  const GUID feature = guid("8836fe87-edb9-4fe3-8dad-05f0d2cd5b40");
  out.push_back({"XGameRuntimeIsFeatureAvailable", feature, feature, {
      {3, "XGameRuntimeIsFeatureAvailable", AM_GDK(feature_available), Z},
  }});
  out.push_back({"XDisplay (HDR)", guid("03f0fe74-fdd9-4e5c-b630-f9339c47acc5"),
                 guid("35f07670-706e-4bfb-9476-090798c5ebf3"), {
      {4, "XDisplayTryEnableHdrMode", AM_GDK(display_try_enable_hdr), Z},
  }});
  out.push_back({"XPackage", guid("af406016-e850-4aa8-a88d-2f3dcb9dac7e"), guid("0b9d556e-ab70-43f4-baa0-9999ac4cb040"), {
      {3, "XPackageGetCurrentProcessPackageIdentifier", AM_GDK(package_identifier)},
      {4, "XPackageIsPackagedProcess", AM_GDK(package_is_packaged), Z},
      {5, "XPackageCreateInstallationMonitor", AM_GDK(package_create_monitor)},
      {6, "XPackageCloseInstallationMonitorHandle", AM_GDK(package_close_monitor), Z},
      {7, "XPackageGetInstallationProgress", AM_GDK(package_progress), Z},
      {9, "XPackageRegisterInstallationProgressChanged", AM_GDK(package_register_progress)},
      {12, "XPackageFindChunkAvailability", AM_GDK(package_chunk_availability)},
      {14, "XPackageChangeChunkInstallOrder", nullptr},
      {16, "XPackageInstallChunksAsync", nullptr},
      {17, "XPackageInstallChunksResult", nullptr},
      {18, "XPackageEstimateDownloadSize", AM_GDK(package_estimate_download)},
      {19, "XPackageUninstallChunks", nullptr},
      {25, "XPackageGetMountPath", nullptr},
      {26, "XPackageCloseMountHandle", nullptr, Z},
      {34, "XPackageUninstallPackage", nullptr, Z},
      {37, "XPackageMountWithUiAsync", nullptr},
      {38, "XPackageMountWithUiResult", nullptr},
      {39, "XPackageEnumeratePackages", AM_GDK(package_enumerate)},
  }});
  out.push_back({"XPersistentLocalStorage", guid("f4faf4d4-2d04-4fce-b3e0-474a713a3e84"),
                 guid("41a4e10c-5a7e-41d9-8c37-37bde62a07d6"), {
      {4, "XPersistentLocalStorageGetPath", AM_GDK(local_storage_path)},
      {5, "XPersistentLocalStorageGetSpaceInfo", AM_GDK(local_storage_space)},
  }});
  const GUID event = guid("bbfbdcc7-bfe7-409b-a5ca-edf054960b4d");
  out.push_back({"XGameEvent", event, event, {
      {3, "XGameEventWrite", AM_GDK(game_event_write)},
  }});
}

}  // namespace am::rt::gdk
