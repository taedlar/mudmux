#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "input_mode.hpp"

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

#include "abstract.hpp"
#include "telnet.hpp"
#include "async/console_worker.h"
#include "mudmux/comm.h"

static void _negotiate_telnet_line_input(comm_abstract_ptr& comm, bool enable) {
    if (comm->ssl)
        return;

    // RFC 1184: LINEMODE option negotiation
    if (enable) {
        // negotiate LINEMODE
        if (comm->caps.telnet_linemode) {
            // client supports LINEMODE, request it
            char lm_mode_request[2] = { 1, 1 }; // LINEMODE MODE: enable EDIT
            comm_telnet_send_subnegotiation(comm, TELOPT_LINEMODE, lm_mode_request, sizeof(lm_mode_request));
        }
        else {
            // fallback to Kludge line mode (using TELOPT_ECHO)
        }
    } else {
        // negotiate CHARACTER mode
        if (comm->caps.telnet_linemode) {
            // client supports LINEMODE, request character mode by turn off local editing
            char lm_mode_request[2] = { 1, 0 }; // LINEMODE MODE: disable local edit mode
            comm_telnet_send_subnegotiation(comm, TELOPT_LINEMODE, lm_mode_request, sizeof(lm_mode_request));
            comm_telnet_send_will(comm, TELOPT_ECHO); // take control of local echo for character mode
        }
        else {
            // fallback to Kludge character mode (using TELOPT_ECHO).
        }
    }
}

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
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
    if (comm->flags & C_LINE_INPUT)
        return comm_set_echo (slot, echo); // already in line input mode
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
            (comm->flags & C_LINE_INPUT) != 0, echo);
        break;
    }
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            _negotiate_telnet_line_input(comm, true);
        }
        break;
    }

    comm->flags |= C_LINE_INPUT;
    return comm_set_echo (slot, echo);
}

bool comm_set_char_input (int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm && slot != COMM_SLOT_CONSOLE)
        return false;
    switch (slot) {
    case COMM_SLOT_CONSOLE:
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
        if (!set_console_input_mode (
            /* set */ ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT,
            /* clear */ ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
        )) {
            return false;
        }
        break;
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            _negotiate_telnet_line_input(comm, false);
        }
        break;
    }
    if (comm)
        comm->flags &= ~(C_LINE_INPUT | C_CLIENT_ECHO);
    return true;
}

bool comm_set_echo (int slot, bool echo) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
    bool result = true;
    switch (slot) {
    case COMM_SLOT_CONSOLE:
        if (echo)
            result = set_console_input_mode (ENABLE_ECHO_INPUT, 0);
        else
            result = set_console_input_mode (0, ENABLE_ECHO_INPUT);
        break;
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            if (comm->ssl)
                break;
            // - when we claim won't echo, the client is expected to echo locally (e.g., for line input)
            // - when we claim will echo, the client is expected to suppress local echo (e.g., for password input)
            if (echo && !(comm->flags & C_CLIENT_ECHO))
                comm_telnet_send_wont(comm, TELOPT_ECHO);
            else if (!echo && (comm->flags & C_CLIENT_ECHO))
                comm_telnet_send_will(comm, TELOPT_ECHO);
        }
        break;
    }
    if (echo)
        comm->flags |= C_CLIENT_ECHO;
    else
        comm->flags &= ~C_CLIENT_ECHO;
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
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
    if (comm->flags & C_LINE_INPUT)
        return comm_set_echo (slot, echo); // already in line input mode
    switch (slot) {
    case COMM_SLOT_CONSOLE:
#ifdef HAVE_TERMIOS_H
        struct termios tio;
        if (tcgetattr (STDIN_FILENO, &tio) != 0) {
            SPDLOG_WARN ("tcgetattr() failed for console stdin: {}", strerror(errno));
            return false;
        }
        if (echo)
            tio.c_lflag |= ICANON;
        else
            tio.c_lflag &= ~ICANON;
        if (echo)
            tio.c_lflag |= ECHO;
        else
            tio.c_lflag &= ~ECHO;
        posix_tcsetattr (STDIN_FILENO, &tio);
#endif
        break;
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            _negotiate_telnet_line_input(comm, true);
        }
        break;
    }

    comm->flags |= C_LINE_INPUT;
    return comm_set_echo (slot, echo);
}

bool comm_set_char_input(int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
    if (!(comm->flags & C_LINE_INPUT))
        return comm_set_echo (slot, false); // already in char input mode, disable echo
    bool telnet_char_mode_negotiated = false;
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
#endif
        break;
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            _negotiate_telnet_line_input(comm, false);
            telnet_char_mode_negotiated = true;
        }
        break;
    }
    comm->flags &= ~(C_LINE_INPUT | (telnet_char_mode_negotiated ? C_CLIENT_ECHO : 0));
    if (telnet_char_mode_negotiated)
        return true;
    return comm_set_echo (slot, false); // disable echo in char input mode
}

bool comm_set_echo (int slot, bool echo) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
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
#endif
        break;
    default:
        if (comm->flags & C_ENABLE_TELNET) {
            if (comm->ssl)
                break;
            // - when we claim won't echo, the client is expected to echo locally (e.g., for line input)
            // - when we claim will echo, the client is expected to suppress local echo (e.g., for password input)
            if (echo && !(comm->flags & C_CLIENT_ECHO))
                comm_telnet_send_wont(comm, TELOPT_ECHO);
            else if (!echo && (comm->flags & C_CLIENT_ECHO))
                comm_telnet_send_will(comm, TELOPT_ECHO);
        }
        break;
    }
    if (echo)
        comm->flags |= C_CLIENT_ECHO;
    else
        comm->flags &= ~C_CLIENT_ECHO;
    return true;
}

#endif
