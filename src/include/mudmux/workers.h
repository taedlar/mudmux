#ifndef MUDMUX_WORKERS_H
#define MUDMUX_WORKERS_H

#include <stdbool.h>
#include <stddef.h>

#include "mudmux/async.h"
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

/**
 * Submit detached application work to the configured worker pool.
 *
 * On success mudmux takes ownership of both closures, clears the caller's
 * handles, invokes @p work on a worker, then invokes @p completion through a
 * serialized non-slot completion lane. Work receives
 * ASYNC_CLOSURE_SCHEDULER_OK. Completion receives that same value when work
 * and its cleanup complete normally, or ASYNC_CLOSURE_SCHEDULER_FAILED when
 * either throws. Neither closure is associated with a communication slot.
 * Both closure handles remain owned by the caller when this function returns
 * false.
 */
MUDMUX_EXPORT bool mudmux_workers_submit(async_closure_t* work, async_closure_t* completion);

/**
 * Hold the current inbound operation while work runs on a worker, then resume
 * it through the same slot's execution gate.
 *
 * This function is valid only from HOOK_MESSAGE_INBOUND or
 * HOOK_TELNET_SUBNEG for the current slot. It records the await request but
 * does not submit work until the initiating hook returns. On success mudmux
 * takes ownership of both closures and clears the caller's handles. The resume
 * closure receives ASYNC_CLOSURE_SCHEDULER_OK or
 * ASYNC_CLOSURE_SCHEDULER_FAILED according to the work closure result. A
 * second await for the same slot is rejected while one is requested or
 * pending; rejected calls retain caller ownership of both closures.
 */
MUDMUX_EXPORT bool mudmux_workers_await(async_closure_t* work, async_closure_t* resume);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_WORKERS_H */
