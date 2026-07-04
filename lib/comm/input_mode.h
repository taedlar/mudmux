#ifndef COMM_INPUT_MODE_H
#define COMM_INPUT_MODE_H

/**
 * @brief Enable cooked line-input mode for the process console stdin.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param echo true to enable local echo, false to disable it.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int comm_set_console_line_input(bool echo);

/**
 * @brief Toggle local echo for the current console stdin mode.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param echo true to enable local echo, false to disable it.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int comm_set_console_echo (bool echo);

/**
 * @brief Toggle single-character mode for the process console stdin.
 * @param ctx Optional console worker context for desired-mode tracking.
 * @param single true to enable single-character mode, false to restore line mode.
 * @returns 1 if stdin is a real console and the mode was updated, otherwise 0.
 */
int comm_set_console_char_input (void);

/**
 * @brief Enable ANSI virtual terminal processing for the process console stdout.
 * @returns 1 if stdout is a real console and the mode was updated, otherwise 0.
 */
int comm_enable_console_virtual_terminal(void);

#endif /* COMM_INPUT_MODE_H */
