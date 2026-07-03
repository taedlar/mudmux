/**
 * @file console_mode.h
 * @brief Shared console mode helpers.
 */

#ifndef CONSOLE_MODE_H
#define CONSOLE_MODE_H

typedef struct console_worker_context_s console_worker_context_t;

/**
 * @brief Enable cooked line-input mode for the process console stdin.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param echo true to enable local echo, false to disable it.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int set_console_input_line_mode(console_worker_context_t* ctx, bool echo);

/**
 * @brief Toggle local echo for the current console stdin mode.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param echo true to enable local echo, false to disable it.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int set_console_input_echo(console_worker_context_t* ctx, bool echo);

/**
 * @brief Toggle single-character mode for the process console stdin.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param single true to enable single-character mode, false to restore line mode.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int set_console_input_single_char(console_worker_context_t* ctx);

/**
 * @brief Enable ANSI virtual terminal processing for the process console stdout.
 * @returns 1 if stdout is a real console and the mode was updated, otherwise 0.
 */
int enable_console_output_ansi(void);

#endif /* CONSOLE_MODE_H */
