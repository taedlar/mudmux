#ifndef CHATROOM_MENU_HPP
#define CHATROOM_MENU_HPP

#include <string>
#include <vector>
#include "mudmux/comm.h"

class Menu {
protected:
    std::string title;
    std::vector<std::string> options;
    int selected_index = -1;
    int cursor_position = 0; // position of the cursor in the options list

public:
    Menu(const std::string& title, int pos = 0) : title(title), cursor_position(pos) {}
    Menu& addOption(const std::string& option) {
        options.push_back(option);
        return *this;
    }
    int getSelectedIndex() const {
        return selected_index;
    }

    void writeMenu (comm_abstract_t* comm) const;
    void receiveCharInput (const std::string& message);
};

#endif  // CHATROOM_MENU_HPP
