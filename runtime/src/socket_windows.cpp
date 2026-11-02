#ifndef _WIN32
#  error "socket_windows.cpp is the Windows-only socket runtime."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "nirqoba/runtime/socket.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

struct SocketResource {
  explicit SocketResource(SOCKET value) : handle(value) {}
  SOCKET handle = INVALID_SOCKET;
  std::mutex operation_mutex;
};

std::mutex registry_mutex;
std::unordered_map<void*, std::shared_ptr<SocketResource>> sockets;
uintptr_t next_socket_token = 1;
std::once_flag winsock_once;
int winsock_status = WSASYSNOTREADY;

bool initialize_winsock() {
  std::call_once(winsock_once, [] {
    WSADATA data{};
    winsock_status = WSAStartup(MAKEWORD(2, 2), &data);
  });
  return winsock_status == 0;
}

std::shared_ptr<SocketResource> find_socket(void* token) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto found = sockets.find(token);
  return found == sockets.end() ? nullptr : found->second;
}

}  // namespace

extern "C" {

void* nrq_socket_connect(const char* host, uint32_t port) {
  if (!host || !*host || port == 0 || port > UINT16_MAX ||
      !initialize_winsock()) return nullptr;
  char service[6]{};
  const int service_length = snprintf(service, sizeof(service), "%u",
                                      static_cast<unsigned>(port));
  if (service_length <= 0 || static_cast<size_t>(service_length) >= sizeof(service))
    return nullptr;

  ADDRINFOA hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  ADDRINFOA* addresses = nullptr;
  if (getaddrinfo(host, service, &hints, &addresses) != 0) return nullptr;
  SOCKET connected = INVALID_SOCKET;
  for (ADDRINFOA* address = addresses; address; address = address->ai_next) {
    SOCKET candidate = ::socket(address->ai_family, address->ai_socktype,
                                address->ai_protocol);
    if (candidate == INVALID_SOCKET) continue;
    if (::connect(candidate, address->ai_addr,
                  static_cast<int>(address->ai_addrlen)) == 0) {
      connected = candidate;
      break;
    }
    closesocket(candidate);
  }
  freeaddrinfo(addresses);
  if (connected == INVALID_SOCKET) return nullptr;

  try {
    auto resource = std::make_shared<SocketResource>(connected);
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (next_socket_token == 0 || next_socket_token == UINTPTR_MAX) {
      closesocket(connected);
      return nullptr;
    }
    void* token = reinterpret_cast<void*>(next_socket_token++);
    if (!sockets.emplace(token, std::move(resource)).second) {
      closesocket(connected);
      return nullptr;
    }
    return token;
  } catch (...) {
    closesocket(connected);
    return nullptr;
  }
}

int64_t nrq_socket_send(void* token, const void* data, size_t length) {
  if (!token || (!data && length != 0) ||
      length > static_cast<size_t>(INT64_MAX)) return -WSAEINVAL;
  std::shared_ptr<SocketResource> resource = find_socket(token);
  if (!resource) return -WSAENOTSOCK;
  std::lock_guard<std::mutex> lock(resource->operation_mutex);
  if (resource->handle == INVALID_SOCKET) return -WSAENOTSOCK;
  size_t sent = 0;
  const auto* bytes = static_cast<const char*>(data);
  while (sent < length) {
    const int request = static_cast<int>(std::min<size_t>(
        length - sent, static_cast<size_t>(INT_MAX)));
    const int result = ::send(resource->handle, bytes + sent, request, 0);
    if (result == SOCKET_ERROR) return -static_cast<int64_t>(WSAGetLastError());
    if (result == 0) return -WSAECONNRESET;
    sent += static_cast<size_t>(result);
  }
  return static_cast<int64_t>(sent);
}

int64_t nrq_socket_send_text(void* token, const char* text) {
  if (!text) text = "";
  return nrq_socket_send(token, text, std::strlen(text));
}

int64_t nrq_socket_receive(void* token, void* destination, size_t capacity) {
  if (!token || (!destination && capacity != 0)) return -WSAEINVAL;
  if (capacity == 0) return 0;
  std::shared_ptr<SocketResource> resource = find_socket(token);
  if (!resource) return -WSAENOTSOCK;
  std::lock_guard<std::mutex> lock(resource->operation_mutex);
  if (resource->handle == INVALID_SOCKET) return -WSAENOTSOCK;
  const int request = static_cast<int>(std::min<size_t>(
      capacity, static_cast<size_t>(INT_MAX)));
  const int result = ::recv(resource->handle,
                            static_cast<char*>(destination), request, 0);
  if (result == SOCKET_ERROR) return -static_cast<int64_t>(WSAGetLastError());
  return static_cast<int64_t>(result);
}

int32_t nrq_socket_close_handle(void* token) {
  if (!token) return WSAENOTSOCK;
  std::shared_ptr<SocketResource> resource;
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto found = sockets.find(token);
    if (found == sockets.end()) return WSAENOTSOCK;
    resource = std::move(found->second);
    sockets.erase(found);
  }
  // Shutdown before waiting for the operation mutex so a blocked receive is
  // woken and close cannot wait forever for an I/O operation on this socket.
  shutdown(resource->handle, SD_BOTH);
  std::lock_guard<std::mutex> lock(resource->operation_mutex);
  if (resource->handle == INVALID_SOCKET) return WSAENOTSOCK;
  const int status = closesocket(resource->handle) == 0 ? 0 : WSAGetLastError();
  resource->handle = INVALID_SOCKET;
  return status;
}

}  // extern "C"
