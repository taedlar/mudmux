#ifndef MUDMUX_H
#define MUDMUX_H

#include "mudmux_export.h"
#include "mudmux/hooks.h"

/* In-process APIs for hosted MUD servers */

#ifdef __cplusplus
extern "C" {
#endif

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

#endif /* MUDMUX_H */
