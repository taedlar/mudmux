#ifndef COMM_INPUT_MODE_H
#define COMM_INPUT_MODE_H

/**
 * @brief Enable (TELNET line mode, termios ICANON or Windows cooked) line-input mode for the
 *      communication slot.
 * @param slot Communication slot to set line input mode. Supports special slots
 *      such as COMM_SLOT_CONSOLE for the process console stdin.
 * @param echo true to enable local echo, false to disable it.
 * @returns true if success, false otherwise.
 */
bool comm_set_line_input (int slot, bool echo);

/**
 * @brief Enable single-character mode for the communication slot.
 * @param slot Communication slot to enable character input mode. Supports special slots
 *      such as COMM_SLOT_CONSOLE for the process console stdin. Client echo is always disabled
 *      by calling this function. If you want to enable echo, call comm_set_echo() immediately
 *      after this function.
 * @returns true if success, false otherwise.
 */
bool comm_set_char_input (int slot);

/**
 * @brief Set or negotiate client echo for the communication slot, without changing the input mode.
 * @param slot Communication slot to set echo mode. Supports special slots such as
 *      COMM_SLOT_CONSOLE for the process console stdin.
 * @param echo true to enable local echo, false to disable it.
 * @returns true if success, false otherwise.
 */
bool comm_set_echo (int slot, bool echo);

#endif /* COMM_INPUT_MODE_H */
