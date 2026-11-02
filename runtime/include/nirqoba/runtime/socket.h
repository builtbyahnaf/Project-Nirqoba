// Deterministically owned Windows TCP socket resources.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* nrq_socket_connect(const char* host, uint32_t port);
int64_t nrq_socket_send(void* socket, const void* data, size_t length);
int64_t nrq_socket_send_text(void* socket, const char* text);
int64_t nrq_socket_receive(void* socket, void* destination, size_t capacity);
int32_t nrq_socket_close_handle(void* socket);

#ifdef __cplusplus
}
#endif
