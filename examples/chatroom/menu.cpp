#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "menu.hpp"
#include "user.hpp"

// ANSI escape codes for terminal control
#define CSI     "\x1B["
#define CLR     CSI "J"                     // Clear from cursor to end of screen
#define CUU     CSI "A"                     // Cursor Up / Up arrow key
#define CUU_(x) fmt::format(CSI "{}A", x)   // Cursor Up by x lines
#define CUD     CSI "B"                     // Cursor Down / Down arrow key
#define CUD_(x) fmt::format(CSI "{}B", x)   // Cursor Down by x lines

// SGR (Select Graphic Rendition) codes for text formatting
#define RESET   CSI "0m"                    // Reset all attributes
#define INV     CSI "7m"                    // Inverse video

void Menu::writeMenu (std::shared_ptr<User> user) const {
    std::string menu_text = CLR "\n";
    for (size_t i = 0; i < options.size(); ++i) {
        if (static_cast<int>(i) == cursor_position) {
            menu_text += " " INV " " + options[i] + " " RESET "\n"; // highlight the selected option
        } else {
            menu_text += "  " + options[i] + "\n";
        }
    }
    menu_text += CUU_(options.size() + 1) + "\r"; // move cursor up to the prompt line
    menu_text += title; // display the menu title and let cursor stay at the end of title for user input
    comm_buffered_write(user->getCommSlot(), menu_text.c_str(), menu_text.size());
}

void Menu::receiveCharInput (const std::string& message) {
    if (message == CUU) { // Up arrow
        SPDLOG_TRACE("Up arrow pressed, cursor_position: {}", cursor_position);
        if (cursor_position > 0) {
            --cursor_position;
        }
    } else if (message == CUD) { // Down arrow
        SPDLOG_TRACE("Down arrow pressed, cursor_position: {}", cursor_position);
        if (cursor_position < static_cast<int>(options.size()) - 1) {
            ++cursor_position;
        }
    } else if (message == " " || message == "\n" || message == "\r") { // Space or Enter
        SPDLOG_TRACE("Option selected, cursor_position: {}", cursor_position);
        // Option selected, handle it as needed
        selected_index = cursor_position;
    } else if (message.length() > 0) {
        char input_char = message[0];
        for (size_t i = 0; i < options.size(); ++i) {
            if (tolower(options[i][0]) == tolower(input_char)) { // match first character of option
                SPDLOG_TRACE("Option selected by character '{}', cursor_position: {}", input_char, i);
                cursor_position = static_cast<int>(i);
                selected_index = cursor_position;
                break;
            }
        }
    }
}
