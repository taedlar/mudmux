#ifndef MUDMUX_WORKERS_H
#define MUDMUX_WORKERS_H

#include <stdbool.h>
#include <stddef.h>

#include "mudmux_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the configured worker thread pool.
 *
 * mudmux_init() starts the pool automatically. This function is provided for
 * applications that explicitly stop and later restart workers before
 * mudmux_deinit().
 *
 * @return true when the pool was started, false when it is already running or
 * could not be started.
 */
MUDMUX_EXPORT bool mudmux_workers_start(void);

/**
 * @brief Stop all worker threads after their queued work completes.
 *
 * mudmux_deinit() always stops workers before tearing down the library. Do
 * not call this function from a worker callback.
 */
MUDMUX_EXPORT void mudmux_workers_stop(void);

/**
 * @brief Return the number of worker threads currently in the pool.
 *
 * This returns zero after workers have been stopped.
 */
MUDMUX_EXPORT size_t mudmux_workers_pool_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_WORKERS_H */
