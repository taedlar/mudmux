#ifndef MUDMUX_MUDMUX_H
#define MUDMUX_MUDMUX_H

#include "mudmux_export.h"

/* In-process APIs for hosted MUD servers */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set mudmux internal logger level.
 *
 * @param level Integer value from spdlog::level::level_enum.
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
 * Console mode is simular to standard input mode, except the server does not exit when
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
 * @brief Shutdown the mudmux server.
 */
MUDMUX_EXPORT void mudmux_shutdown (void);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_MUDMUX_H */
