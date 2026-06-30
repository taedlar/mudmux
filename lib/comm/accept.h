#ifndef COMM_ACCEPT_H
#define COMM_ACCEPT_H

#include "mudmux_export.h"
#include "async/async_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

MUDMUX_EXPORT int comm_accept (async_runtime_t* runtime, const char* accept_name);

#ifdef __cplusplus
}
#endif

#endif /* COMM_ACCEPT_H */
