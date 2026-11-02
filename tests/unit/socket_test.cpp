#include "test_harness.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "nirqoba/runtime/socket.h"

TEST(SocketTest, ConnectSendReceiveAndRejectStaleHandle) {
  WSADATA startup{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &startup), 0);
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_NE(listener, INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)), 0);
  int address_size = sizeof(address);
  ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                        &address_size), 0);
  ASSERT_EQ(::listen(listener, 1), 0);

  std::atomic<bool> server_ok = true;
  std::thread server([&] {
    SOCKET peer = ::accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) {
      server_ok = false;
      return;
    }
    std::array<char, 4> request{};
    int received = 0;
    while (received < static_cast<int>(request.size())) {
      const int result = ::recv(peer, request.data() + received,
                                static_cast<int>(request.size()) - received, 0);
      if (result <= 0) {
        server_ok = false;
        break;
      }
      received += result;
    }
    if (std::memcmp(request.data(), "ping", request.size()) != 0)
      server_ok = false;
    if (::send(peer, "pong", 4, 0) != 4) server_ok = false;
    closesocket(peer);
  });

  void* handle = nrq_socket_connect("127.0.0.1", ntohs(address.sin_port));
  bool client_ok = handle != nullptr;
  if (handle) {
    client_ok = nrq_socket_send_text(handle, "ping") == 4;
    std::array<char, 4> response{};
    const int64_t received = nrq_socket_receive(
        handle, response.data(), response.size());
    client_ok = client_ok && received == 4 &&
        std::memcmp(response.data(), "pong", response.size()) == 0;
  }
  if (!handle) closesocket(listener);
  server.join();
  const int32_t close_status = handle ? nrq_socket_close_handle(handle) : -1;
  const int32_t stale_status = handle ? nrq_socket_close_handle(handle) : 0;
  if (handle) closesocket(listener);
  WSACleanup();

  ASSERT_TRUE(client_ok);
  ASSERT_TRUE(server_ok.load());
  ASSERT_EQ(close_status, int32_t{0});
  ASSERT_NE(stale_status, int32_t{0});
}

TEST(SocketTest, CloseUnblocksAConcurrentReceive) {
  WSADATA startup{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &startup), 0);
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_NE(listener, INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)), 0);
  int address_size = sizeof(address);
  ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                        &address_size), 0);
  ASSERT_EQ(::listen(listener, 1), 0);

  std::thread server([&] {
    SOCKET peer = ::accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) return;
    char byte = 0;
    ::recv(peer, &byte, 1, 0);
    closesocket(peer);
  });
  void* handle = nrq_socket_connect("127.0.0.1", ntohs(address.sin_port));
  std::atomic<bool> receiving = false;
  int64_t receive_result = 0;
  std::thread receiver;
  if (handle) {
    receiver = std::thread([&] {
      receiving.store(true, std::memory_order_release);
      char byte = 0;
      receive_result = nrq_socket_receive(handle, &byte, 1);
    });
    while (!receiving.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } else {
    closesocket(listener);
  }
  const int32_t close_status = handle ? nrq_socket_close_handle(handle) : -1;
  if (receiver.joinable()) receiver.join();
  server.join();
  if (handle) closesocket(listener);
  WSACleanup();

  ASSERT_NE(handle, nullptr);
  ASSERT_EQ(close_status, int32_t{0});
  ASSERT_TRUE(receive_result <= 0);
  ASSERT_TRUE(nrq_socket_send_text(handle, "stale") < 0);
}

TEST(SocketTest, SendProgressesWhileAnotherThreadReceives) {
  WSADATA startup{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &startup), 0);
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_NE(listener, INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)), 0);
  int address_size = sizeof(address);
  ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                        &address_size), 0);
  ASSERT_EQ(::listen(listener, 1), 0);

  std::atomic<bool> server_ok = true;
  std::thread server([&] {
    SOCKET peer = ::accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) {
      server_ok = false;
      return;
    }
    std::array<char, 4> request{};
    int received = 0;
    while (received < static_cast<int>(request.size())) {
      const int result = ::recv(peer, request.data() + received,
                                static_cast<int>(request.size()) - received, 0);
      if (result <= 0) {
        server_ok = false;
        break;
      }
      received += result;
    }
    if (std::memcmp(request.data(), "ping", request.size()) != 0)
      server_ok = false;
    if (::send(peer, "pong", 4, 0) != 4) server_ok = false;
    closesocket(peer);
  });

  void* handle = nrq_socket_connect("127.0.0.1", ntohs(address.sin_port));
  std::atomic<bool> receiving = false;
  int64_t receive_result = 0;
  std::array<char, 4> response{};
  std::thread receiver;
  if (handle) {
    receiver = std::thread([&] {
      receiving.store(true, std::memory_order_release);
      receive_result = nrq_socket_receive(
          handle, response.data(), response.size());
    });
    while (!receiving.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } else {
    closesocket(listener);
  }
  const int64_t sent = handle ? nrq_socket_send_text(handle, "ping") : -1;
  if (receiver.joinable()) receiver.join();
  server.join();
  if (handle) closesocket(listener);
  const int32_t close_status = handle ? nrq_socket_close_handle(handle) : -1;
  WSACleanup();

  ASSERT_NE(handle, nullptr);
  ASSERT_EQ(sent, int64_t{4});
  ASSERT_EQ(receive_result, int64_t{4});
  ASSERT_TRUE(server_ok.load());
  ASSERT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
  ASSERT_EQ(close_status, int32_t{0});
}
