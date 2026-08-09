#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "menu.hpp"

#define CSI "\x1B["
#define CLR CSI "J"         /* (ED n=2) Clear from cursor to end of screen */
#define CUU(x) fmt::format(CSI "{}A", x) // Cursor Up

static void write_slot_text(int slot, const std::string& text) {
    comm_buffered_write(slot, text.c_str(), text.size());
}

void Menu::writeMenu (int slot) const {
    if (slot < 0)
        return;
    write_slot_text(slot, std::string(CLR) + "\n"); // clear from cursor to end of screen
    for (size_t i = 0; i < options.size(); ++i) {
        if (static_cast<int>(i) == cursor_position) {
            write_slot_text(slot, "> " + options[i] + "\n"); // highlight the selected option
        } else {
            write_slot_text(slot, "  " + options[i] + "\n");
        }
    }
    write_slot_text(slot, CUU(options.size() + 1) + "\r"); // move cursor up to the prompt line
    write_slot_text(slot, title); // display the menu title and let cursor stay at the end of title for user input
}

void Menu::receiveCharInput (int slot, const std::string& message) {
    if (message == "\x1B[A") { // Up arrow
        SPDLOG_DEBUG("Up arrow pressed, cursor_position: {}", cursor_position);
        if (cursor_position > 0) {
            --cursor_position;
        }
    } else if (message == "\x1B[B") { // Down arrow
        SPDLOG_DEBUG("Down arrow pressed, cursor_position: {}", cursor_position);
        if (cursor_position < static_cast<int>(options.size()) - 1) {
            ++cursor_position;
        }
    } else if (message == " " || message == "\n" || message == "\r") { // Space or Enter
        SPDLOG_DEBUG("Option selected, cursor_position: {}", cursor_position);
        // Option selected, handle it as needed
        selected_index = cursor_position;
        write_slot_text(slot, "\r\x1B[J"); // clear the line and move cursor to the beginning
    } else if (message.length() > 0) {
        char input_char = message[0];
        for (size_t i = 0; i < options.size(); ++i) {
            if (tolower(options[i][0]) == tolower(input_char)) {
                cursor_position = static_cast<int>(i);
                selected_index = cursor_position;
                write_slot_text(slot, "\r\x1B[J"); // clear the line and move cursor to the beginning
                break;
            }
        }
    }
}
