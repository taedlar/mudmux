#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "menu.hpp"

#define CSI "\x1B["
#define CLR CSI "J"         /* (ED n=2) Clear from cursor to end of screen */
#define CUU(x) fmt::format(CSI "{}A", x) // Cursor Up

void Menu::writeMenu (comm_abstract_t* comm) const {
    if (!comm)
        return;
    *comm << CLR << "\n\r"; // clear from cursor to end of screen
    for (size_t i = 0; i < options.size(); ++i) {
        if (static_cast<int>(i) == cursor_position) {
            *comm << "> " << options[i] << "\n\r"; // highlight the selected option
        } else {
            *comm << "  " << options[i] << "\n\r";
        }
    }
    *comm << CUU(options.size() + 1) << "\r"; // move cursor up to the prompt line
    *comm << title; // display the menu title and let cursor stay at the end of title for user input
}

void Menu::receiveCharInput (comm_abstract_t* comm, const std::string& message) {
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
        *comm << "\r\x1B[J"; // clear the line and move cursor to the beginning
    }
}
