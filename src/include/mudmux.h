#ifndef MUDMUX_H
#define MUDMUX_H

/* In-process APIs for hosted MUD servers */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the MUD server library.
 * @param config_yaml YAML configuration contents, or NULL to use defaults.
 * @return true on success, false on failure.
 */
bool mudmux_init (const char* config_yaml);

/**
 * @brief Run the MUD server.
 * @param context Pointer to the context, or NULL to use defaults.
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on failure.
 */
int mudmux_run (void* context);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_H */
