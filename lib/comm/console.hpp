#ifndef COMM_CONSOLE_HPP
#define COMM_CONSOLE_HPP

#include "async/async_runtime.h"

bool comm_init_console (async_runtime_t *runtime);
void comm_signal_console_eof (async_runtime_t *runtime);
void comm_shutdown_console (async_runtime_t *runtime);

/**
 * @brief Enables virtual terminal (tty or ANSI/VT100) output processing for the communication slot.
 * @param slot Communication slot to enable virtual terminal processing. Supports special slots
 *      such as COMM_SLOT_CONSOLE (for the process console stdout). This is usually a no-op on
 *      Linux/Unix, but on Windows it enables ANSI escape sequence processing for the stdout.
 * @returns true if success, false otherwise.
 */
bool comm_enable_virtual_terminal (int slot);

int comm_process_console_input (async_runtime_t *runtime, bool allow_reconnect = false);

#endif /* COMM_CONSOLE_HPP */
