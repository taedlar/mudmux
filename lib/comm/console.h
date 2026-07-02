#ifndef COMM_CONSOLE_H
#define COMM_CONSOLE_H

#include "async/async_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

bool comm_init_console (async_runtime_t *runtime);
void comm_shutdown_console (async_runtime_t *runtime);

int comm_process_console_input (async_runtime_t *runtime, bool allow_reconnect = false);

#ifdef __cplusplus
}
#endif

#endif // COMM_CONSOLE_H
