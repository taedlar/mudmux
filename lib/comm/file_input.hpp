#ifndef COMM_FILE_INPUT_HPP
#define COMM_FILE_INPUT_HPP

#include "async/async_runtime.h"

#define FILE_INPUT_COMPLETION_KEY(slot) (((uintptr_t)0xFFF0 << 16) | ((slot) & 0xFFFF))
#define FILE_INPUT_SLOT_FROM_KEY(key) ((key) & 0xFFFF)
#define IS_FILE_INPUT_COMPLETION_KEY(key) ((((key) >> 16) & 0xFFFF) == 0xFFF0)

/**
 * Initialize asynchronous file input (when file input is set up on-demand).
 * Starts the file reader thread without draining the queue.
 * @param runtime The async runtime
 * @param slot The communication slot for this file input
 */
bool comm_init_async_file_input (async_runtime_t *runtime, int slot);

void comm_shutdown_async_file_input (void);

/**
 * Check if console slot is using file input instead of stdin/console worker.
 * @returns true if file input is active, false otherwise
 */
bool comm_has_file_inputs (void);

int comm_process_file_input (async_runtime_t *runtime, int slot, const io_event_t* event);

#endif /* COMM_FILE_INPUT_HPP */
