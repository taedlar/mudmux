#ifndef MUDMUX_MUDMUX_H
#define MUDMUX_MUDMUX_H

#include <stdbool.h>
#include "mudmux_export.h"
#include "mudmux/async.h"
#include "mudmux/hooks.h"

/* In-process APIs for hosted MUD servers */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set mudmux internal logger level.
 *
 * @param level mudmux integer severity value.
 */
MUDMUX_EXPORT void mudmux_set_log_level (int level);

/**
 * @brief Enable or disable standard input for the mudmux server.
 *
 * @param enable true to enable standard input, false to disable.
 */
MUDMUX_EXPORT void mudmux_enable_standard_input (bool enable);

/**
 * @brief Enable or disable console support for the mudmux server.
 * Console mode is similar to standard input mode, except the server does not exit when
 * EOF is received on stdin or disconnected by the MUD server. Instead, the console can be
 * re-connected after EOF or disconnection if any input is received on stdin (e.g., pressing
 * ENTER on the keyboard).
 * 
 * @param enable true to enable console support, false to disable.
 */
MUDMUX_EXPORT void mudmux_enable_console (bool enable);

/**
 * @brief Initialize the mudmux server library.
 * @param config_yaml YAML configuration contents, or NULL to use defaults.
 * @return true on success, false on failure.
 */
MUDMUX_EXPORT bool mudmux_init (const char* config_yaml);

/** Return mudmux's initialized timer event. Use mudmux_trigger_timer() to publish flags. */
MUDMUX_EXPORT async_event_t* mudmux_get_timer_event(void);

/**
 * Accumulate flags and signal mudmux's timer event. HOOK_TIMER receives the
 * bitwise OR of pending flags as its positive int msg. Repeated bits coalesce;
 * triggers during a callback accumulate for a subsequent invocation.
 * Zero and flags containing the highest unsigned int bit are rejected, keeping
 * msg 0 and -1 reserved for mudmux_run() startup and shutdown notifications.
 * Returns false for invalid flags or when the timer event is unavailable.
 */
MUDMUX_EXPORT bool mudmux_trigger_timer(unsigned int flags);

/**
 * @brief Deinitialize the mudmux server library.
 */
MUDMUX_EXPORT void mudmux_deinit (void);

/**
 * @brief Run the mudmux server.
 * @param context Pointer to the context, or NULL to use defaults.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on failure.
 */
MUDMUX_EXPORT int mudmux_run (void* context);

/**
 * @brief Request shutdown of the mudmux server.
 *
 * Returns immediately, including when called from a worker hook. mudmux_run()
 * joins worker executions and their completions before destroying the async
 * runtime and returning.
 */
MUDMUX_EXPORT void mudmux_shutdown (void);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_MUDMUX_H */
