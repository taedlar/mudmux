#ifndef CHATROOM_USER_HPP
#define CHATROOM_USER_HPP

#include <memory>
#include <string>
#include <vector>
#include "mudmux/comm.h"
#include "menu.hpp"

enum class UserState {
    Disconnected,
    Connected,
    PreLogin,
    LoggedIn
};

class User: public std::enable_shared_from_this<User> {
private:
    std::string username;
    int comm_slot;
    UserState state;
    void (User::* inbound_handler)(const std::string& message) = nullptr;
    void (User::* prompt_handler)() = nullptr;

    std::unique_ptr<Menu> menu;

public:
    static std::vector<std::shared_ptr<User>> slots; // mapping of comm slots to User instances

    User(int slot) : comm_slot(slot), state(UserState::Disconnected) {}

    const std::string& getUsername() const {
        return username;
    }

    UserState getState() const {
        return state;
    }

    inline int getCommSlot() const {
        return comm_slot;
    }

    void closeComm() {
        comm_close(nullptr, comm_slot);
    }

    void setCharInput () {
        if (comm_slot >= 0)
            comm_set_char_input (comm_slot);
    }

    void setLineInput (bool echo = true) {
        if (comm_slot >= 0)
            comm_set_line_input (comm_slot, echo);
    }

    void doMenu (std::unique_ptr<Menu> new_menu, void (User::* handler)(const std::string& message)) {
        menu = std::move(new_menu);
        if (menu) {
            prompt_handler = &User::promptCurrentMenu;
            inbound_handler = handler;
        }
    }

    // hook functions
    void logon();
    void disconnect();
    void prompt();
    void dispatchInboundMessage(const std::string& message);

    // inbound handlers
    void receiveUsername (const std::string& name);
    void receiveExitConfirmation (const std::string& message);

    // prompt handlers
    void promptCurrentMenu() {
        if (menu) {
            menu->writeMenu(comm_slot);
        }
    }
};

#endif  // CHATROOM_USER_HPP
