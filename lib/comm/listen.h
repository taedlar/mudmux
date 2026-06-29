#ifndef COMM_SOCKET_H
#define COMM_SOCKET_H

#include "async/async_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

bool comm_init_listening_port (async_runtime_t *runtime, int port, void *ctx);

#ifdef __cplusplus
}
#endif

#endif // COMM_SOCKET_H
