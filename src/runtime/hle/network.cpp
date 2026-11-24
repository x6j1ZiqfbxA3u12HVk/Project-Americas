// Network policy: the title stays offline. Connections, name lookups and datagrams to anything
// but this machine fail the way an unplugged network fails. (The game's crash reporter would
// otherwise upload crash dumps.)

// The title imports the deprecated gethostbyname; we wrap it, so its header must declare it.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <cstring>
#include <string_view>

#include "core/text.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::hle {
namespace {

bool is_loopback(const sockaddr* a, int len) {
  if (!a) return false;
  if (a->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
    return (ntohl(reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr) >> 24) == 127;
  }
  if (a->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
    const in6_addr& v6 = reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr;
    static const in6_addr loop = IN6ADDR_LOOPBACK_INIT;
    return std::memcmp(&v6, &loop, sizeof v6) == 0;
  }
  return false;
}

bool is_local_name(std::string_view name) {
  return name.empty() || iequals_ascii(name, "localhost") || name == "127.0.0.1" || name == "::1";
}

void refused(const char* what, std::string_view target) {
  static volatile long count = 0;
  if (InterlockedIncrement(&count) <= 20) logf("network: %s %.*s refused (offline)", what, static_cast<int>(target.size()), target.data());
}

int WSAAPI my_connect(SOCKET s, const sockaddr* name, int len) {
  if (is_loopback(name, len)) return connect(s, name, len);
  refused("connect", "");
  WSASetLastError(WSAENETUNREACH);
  return SOCKET_ERROR;
}

int WSAAPI my_WSAConnect(SOCKET s, const sockaddr* name, int len, LPWSABUF caller, LPWSABUF callee, LPQOS sqos,
                         LPQOS gqos) {
  if (is_loopback(name, len)) return WSAConnect(s, name, len, caller, callee, sqos, gqos);
  refused("WSAConnect", "");
  WSASetLastError(WSAENETUNREACH);
  return SOCKET_ERROR;
}

BOOL PASCAL my_WSAConnectByNameW(SOCKET, LPWSTR node, LPWSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR,
                                 const timeval*, LPWSAOVERLAPPED) {
  refused("WSAConnectByName", node ? narrow(node) : std::string());
  WSASetLastError(WSAHOST_NOT_FOUND);
  return FALSE;
}

BOOL PASCAL my_WSAConnectByNameA(SOCKET, LPCSTR node, LPCSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR,
                                 const timeval*, LPWSAOVERLAPPED) {
  refused("WSAConnectByName", node ? node : "");
  WSASetLastError(WSAHOST_NOT_FOUND);
  return FALSE;
}

int WSAAPI my_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
  if (is_loopback(to, tolen)) return sendto(s, buf, len, flags, to, tolen);
  refused("sendto", "");
  WSASetLastError(WSAENETUNREACH);
  return SOCKET_ERROR;
}

int WSAAPI my_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen,
                        LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
  if (is_loopback(to, tolen)) return WSASendTo(s, bufs, count, sent, flags, to, tolen, ov, done);
  refused("WSASendTo", "");
  WSASetLastError(WSAENETUNREACH);
  return SOCKET_ERROR;
}

INT WSAAPI my_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
  if (is_local_name(node ? node : "")) return getaddrinfo(node, service, hints, result);
  refused("getaddrinfo", node);
  if (result) *result = nullptr;
  return WSAHOST_NOT_FOUND;
}

INT WSAAPI my_GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
  const std::string n = node ? narrow(node) : std::string();
  if (is_local_name(n)) return GetAddrInfoW(node, service, hints, result);
  refused("GetAddrInfoW", n);
  if (result) *result = nullptr;
  return WSAHOST_NOT_FOUND;
}

INT WSAAPI my_GetAddrInfoExW(PCWSTR node, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW*, PADDRINFOEXW* result, timeval*,
                             LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE) {
  refused("GetAddrInfoExW", node ? narrow(node) : std::string());
  if (result) *result = nullptr;
  return WSAHOST_NOT_FOUND;
}

hostent* WSAAPI my_gethostbyname(const char* name) {
  if (is_local_name(name ? name : "")) return gethostbyname(name);
  refused("gethostbyname", name ? name : "");
  WSASetLastError(WSAHOST_NOT_FOUND);
  return nullptr;
}

HINTERNET WINAPI my_WinHttpConnect(HINTERNET, LPCWSTR server, INTERNET_PORT, DWORD) {
  refused("WinHttpConnect", server ? narrow(server) : std::string());
  SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
  return nullptr;
}

BOOL WINAPI my_WinHttpSendRequest(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) {
  refused("WinHttpSendRequest", "");
  SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
  return FALSE;
}

#define AM_WRAP(name) Function{nullptr, #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void add_network(std::vector<Function>& out) {
  out.insert(out.end(), {
      AM_WRAP(connect), AM_WRAP(WSAConnect), AM_WRAP(WSAConnectByNameW), AM_WRAP(WSAConnectByNameA),
      AM_WRAP(sendto), AM_WRAP(WSASendTo), AM_WRAP(getaddrinfo), AM_WRAP(GetAddrInfoW), AM_WRAP(GetAddrInfoExW),
      AM_WRAP(gethostbyname), AM_WRAP(WinHttpConnect), AM_WRAP(WinHttpSendRequest),
      // Opening a session touches nothing outside: the host's own, so later calls fail cleanly.
      Function{nullptr, "WinHttpOpen", reinterpret_cast<void*>(&::WinHttpOpen), true},
  });
}

}  // namespace am::rt::hle
