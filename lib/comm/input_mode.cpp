#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "input_mode.h"

#include "async/console_worker.h"

#ifdef _WIN32

static int get_console_input_mode(HANDLE *handle, DWORD *mode) {
  *handle = GetStdHandle(STD_INPUT_HANDLE);
  if (*handle == INVALID_HANDLE_VALUE || *handle == NULL)
    return 0;

  if (!GetConsoleMode(*handle, mode))
    return 0;

  return 1;
}

/**
 * @brief Apply a bit-delta to the current console input mode.
 *
 * Reads the current mode, sets @p set_bits and clears @p clear_bits, then
 * writes the result back.  Only the bits that actually need to change are
 * touched, so unrelated flags set by the terminal (e.g. ENABLE_QUICK_EDIT_MODE
 * under Windows Terminal / ConPTY) are preserved, avoiding ERROR_INVALID_PARAMETER.
 */
static int set_console_input_mode(DWORD set_bits, DWORD clear_bits) {
  HANDLE handle;
  DWORD current_mode;
  DWORD new_mode;

  if (!get_console_input_mode (&handle, &current_mode))
    return 0;

  new_mode = (current_mode | set_bits) & ~clear_bits;

  if (!SetConsoleMode (handle, new_mode))
    {
      SPDLOG_WARN ("SetConsoleMode() failed for console stdin: {}", GetLastError());
      return 0;
    }

  if ((clear_bits & ENABLE_LINE_INPUT) && (current_mode & ENABLE_LINE_INPUT))
    {
      /* Inject a key-DOWN ESC event so that dwCtrlWakeupMask (bit 27) fires inside
       * ReadConsoleW and unblocks it.  Key-up events do not produce characters and
       * therefore never trigger the wakeup mask. */
      INPUT_RECORD esc_input;
      memset(&esc_input, 0, sizeof(esc_input));
      esc_input.EventType = KEY_EVENT;
      esc_input.Event.KeyEvent.bKeyDown = TRUE;
      esc_input.Event.KeyEvent.wRepeatCount = 1;
      esc_input.Event.KeyEvent.wVirtualKeyCode = VK_ESCAPE;
      esc_input.Event.KeyEvent.uChar.UnicodeChar = L'\x1B';
      DWORD written = 0;
      WriteConsoleInputW (handle, &esc_input, 1, &written);
    }

  return 1;
}

/**
 * @brief Switch to line (cooked) input mode, optionally enabling echo.
 */
int comm_set_console_line_input (bool echo) {
  DWORD set_bits = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT
                   | (echo ? ENABLE_ECHO_INPUT : 0);
  DWORD clear_bits = (echo ? 0 : ENABLE_ECHO_INPUT) | ENABLE_VIRTUAL_TERMINAL_INPUT;
  return set_console_input_mode(set_bits, clear_bits);
}

int comm_set_console_echo (bool echo) {
  DWORD set_bits = echo ? ENABLE_ECHO_INPUT : 0;
  DWORD clear_bits = echo ? 0 : ENABLE_ECHO_INPUT;
  return set_console_input_mode(set_bits, clear_bits);
}

/**
 * @brief Switch to single-character (raw) input mode, disabling line input.
 * 
 * Also enables Windows 10 ANSI processing to allow reading of virtual terminal sequences for special keys.
 * @see https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
 */
int comm_set_console_char_input () {
  return set_console_input_mode (
    /* set */ ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT,
    /* clear */ ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
  );
}

int comm_enable_console_virtual_terminal(void) {
  HANDLE handle;
  DWORD mode;

  handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == NULL)
    return 0;

  if (!GetConsoleMode(handle, &mode))
    return 0;

  SetConsoleOutputCP(CP_UTF8);
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
  if (!SetConsoleMode(handle, mode))
    {
      SPDLOG_WARN ("SetConsoleMode() failed for console stdout: {}", GetLastError());
      return 0;
    }

  return 1;
}
#else /* Non-Windows (POSIX) */

#ifdef HAVE_TERMIOS_H
#include <termios.h>

/**
 * @brief Apply terminal settings without data loss for pipes.
 *
 * Real TTY: Use TCSAFLUSH to discard stale input (security).
 * Pipe:     Use TCSANOW to preserve all data (testbot).
 */
static void posix_tcsetattr(int fd, struct termios *tio) {
  int action = isatty(fd) ? TCSAFLUSH : TCSANOW;
  tcsetattr(fd, action, tio);
}
#endif /* HAVE_TERMIOS_H */

int comm_set_console_line_input (bool echo) {
#ifdef HAVE_TERMIOS_H
  struct termios tio;
  if (tcgetattr(STDIN_FILENO, &tio) != 0)
    return 0;
  if (echo)
    tio.c_lflag |= ICANON;
  else
    tio.c_lflag &= ~ICANON;
  if (echo)
    tio.c_lflag |= ECHO;
  else
    tio.c_lflag &= ~ECHO;
  posix_tcsetattr(STDIN_FILENO, &tio);
  return 1;
#else
  (void)echo;
  return 0;
#endif
}

int comm_set_console_echo (bool echo) {
#ifdef HAVE_TERMIOS_H
  struct termios tio;
  if (tcgetattr(STDIN_FILENO, &tio) != 0)
    return 0;
  if (echo)
    tio.c_lflag |= ECHO;
  else
    tio.c_lflag &= ~ECHO;
  posix_tcsetattr(STDIN_FILENO, &tio);
  return 1;
#else
  (void)echo;
  return 0;
#endif
}

int comm_set_console_char_input() {
#ifdef HAVE_TERMIOS_H
  {
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) != 0)
      return 0;
    /* disable canonical mode and echo: input character is immediately available for read() */
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_cc[VMIN] = 0;  /* use polling as like O_NONBLOCK was set */
    tio.c_cc[VTIME] = 0; /* no timeout */
    posix_tcsetattr(STDIN_FILENO, &tio);
    return 1;
  }
#else
  return 0;
#endif
}

int comm_enable_console_virtual_terminal(void) {
  return 0;
}
#endif
