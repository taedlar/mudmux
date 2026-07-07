#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "input_mode.h"

#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "abstract.h"
#include "async/console_worker.h"
#include "mudmux/comm.h"

#ifdef _WIN32

static int get_console_input_mode (HANDLE *handle, DWORD *mode) {
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
static bool set_console_input_mode (DWORD set_bits, DWORD clear_bits) {
    HANDLE handle;
    DWORD current_mode;
    DWORD new_mode;

    if (!get_console_input_mode (&handle, &current_mode))
        return false;

    new_mode = (current_mode | set_bits) & ~clear_bits;

    if (!SetConsoleMode (handle, new_mode)) {
        SPDLOG_WARN ("SetConsoleMode() failed for console stdin: {}", GetLastError());
        return false;
    }

    if ((clear_bits & ENABLE_LINE_INPUT) && (current_mode & ENABLE_LINE_INPUT)) {
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

    return true;
}

bool comm_set_line_input (int slot, bool echo) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
    auto comm = comm_abstract_get(slot);
    switch (slot) {
    case COMM_SLOT_CONSOLE: {
        /*
         * On Windows we use cooked mode (ENABLE_LINE_INPUT + ENABLE_PROCESSED_INPUT + ENABLE_ECHO_INPUT)
         * to get canonical line input with echo. This is the typical behavior a MUD server would expect
         * from a terminal.
         *
         * See https://learn.microsoft.com/en-us/windows/console/high-level-console-modes 
         */
        DWORD set_bits = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | (echo ? ENABLE_ECHO_INPUT : 0);
        DWORD clear_bits = ENABLE_VIRTUAL_TERMINAL_INPUT | (echo ? 0 : ENABLE_ECHO_INPUT);
        if (!set_console_input_mode (set_bits, clear_bits))
            return false;
        SPDLOG_DEBUG ("console input mode set: C_LINE_INPUT was {}, echo={}",
            (comm ? (comm_get_flags(comm) & C_LINE_INPUT) != 0 : false), echo);
        break;
    }
    default:
        return false;
    }

    if (comm)
        comm_set_flags (comm, C_LINE_INPUT);
    return true;
}

bool comm_set_char_input (int slot) {
    /*
     * On Windows we use ENABLE_PROCESSED_INPUT + ENABLE_VIRTUAL_TERMINAL_INPUT
     * to get character-at-a-time input without echo.
     *
     * - Ctrl-C still works as a signal to terminate the process (ENABLE_PROCESSED_INPUT)
     * - ANSI escape sequences are recognized to support special keys (ENABLE_VIRTUAL_TERMINAL_INPUT)
     * - Echo is disabled (clears ENABLE_ECHO_INPUT) to prevent ANSI escape sequences from being printed
     *
     * See https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
     */
    bool ret = set_console_input_mode (
        /* set */ ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT,
        /* clear */ ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
    );
    if (ret) {
        std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
        auto comm = comm_abstract_get(slot);
        if (comm)
            comm_clear_flags (comm, C_LINE_INPUT | C_CLIENT_ECHO);
    }
    return ret;
}

bool comm_set_echo (int slot, bool echo) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
    auto comm = comm_abstract_get(slot);
    bool result = false;
    switch (slot) {
    case COMM_SLOT_CONSOLE:
        if (echo)
            result = set_console_input_mode (ENABLE_ECHO_INPUT, 0);
        else
            result = set_console_input_mode (0, ENABLE_ECHO_INPUT);
        break;
        if (!result)
            return false;
        break;
    default:
        return false;
    }
    if (comm) {
        if (echo)
            comm_set_flags (comm, C_CLIENT_ECHO);
        else
            comm_clear_flags (comm, C_CLIENT_ECHO);
    }
    return result;
}

#else
/* Non-Windows (POSIX) */

#ifdef HAVE_TERMIOS_H
/**
 * @brief Apply terminal settings without data loss for pipes.
 *
 * Real TTY: Use TCSAFLUSH to discard stale input (security).
 * Pipe:     Use TCSANOW to preserve all data (testbot).
 */
static void posix_tcsetattr(int fd, struct termios *tio) {
    int action = isatty(fd) ? TCSAFLUSH : TCSANOW;
    tcsetattr (fd, action, tio);
}
#endif /* HAVE_TERMIOS_H */

bool comm_set_line_input (int slot, bool echo) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
    auto comm = comm_abstract_get (slot);
    switch (slot) {
    case COMM_SLOT_CONSOLE:
#ifdef HAVE_TERMIOS_H
        struct termios tio;
        if (tcgetattr (STDIN_FILENO, &tio) != 0)
            return false;
        if (echo)
            tio.c_lflag |= ICANON;
        else
            tio.c_lflag &= ~ICANON;
        if (echo)
            tio.c_lflag |= ECHO;
        else
            tio.c_lflag &= ~ECHO;
        posix_tcsetattr (STDIN_FILENO, &tio);
        break;
#else
        (void)echo;
        return false;
#endif
    default:
        return false;
    }

    if (comm)
        comm_set_flags (comm, C_LINE_INPUT);
    return true;
}

bool comm_set_char_input(int slot) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
    auto comm = comm_abstract_get(slot);
    switch (slot) {
    case COMM_SLOT_CONSOLE:
#ifdef HAVE_TERMIOS_H
        struct termios tio;
        if (tcgetattr (STDIN_FILENO, &tio) != 0)
            return false;
        /* disable canonical mode and echo: input character is immediately available for read() */
        tio.c_lflag &= ~(ICANON | ECHO);
        tio.c_cc[VMIN] = 0;  /* use polling as like O_NONBLOCK was set */
        tio.c_cc[VTIME] = 0; /* no timeout */
        posix_tcsetattr (STDIN_FILENO, &tio);
        break;
#else
        (void)slot;
        return false;
#endif
    default:
        return false;
    }
    if (comm)
        comm_clear_flags (comm, C_LINE_INPUT | C_CLIENT_ECHO);
    return true;
}

bool comm_set_echo (int slot, bool echo) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);
    auto comm = comm_abstract_get (slot);
    switch (slot) {
    case COMM_SLOT_CONSOLE:
#ifdef HAVE_TERMIOS_H
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) != 0)
            return false;
        if (echo)
            tio.c_lflag |= ECHO;
        else
            tio.c_lflag &= ~ECHO;
        posix_tcsetattr(STDIN_FILENO, &tio);
        break;
#else
        (void)echo;
        return false;
#endif
    default:
        return false;
    }
    if (comm) {
        if (echo)
            comm_set_flags (comm, C_CLIENT_ECHO);
        else
            comm_clear_flags (comm, C_CLIENT_ECHO);
    }
    return true;
}

#endif
