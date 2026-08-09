#ifndef CHATROOM_USER_HPP
#define CHATROOM_USER_HPP

#include <chrono>
#include <memory>
#include <mutex>
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
    static std::recursive_mutex users_mutex;
    static std::vector<std::shared_ptr<User>> slots;

    std::string username;
    int comm_slot;
    UserState state;
    std::chrono::steady_clock::time_point idle_time;
    void (User::* inbound_handler)(const std::string& message) = nullptr;
    void (User::* prompt_handler)() = nullptr;

    std::unique_ptr<Menu> menu;

public:
    User(int slot)
        : comm_slot(slot), state(UserState::Disconnected), idle_time(std::chrono::steady_clock::now()) {}

    /** Create and register a user for a communication slot. */
    static std::shared_ptr<User> connect(int slot);

    /** Return a stable reference to the user currently registered for a slot. */
    static std::shared_ptr<User> find(int slot);

    /** Remove and return the user currently registered for a slot. */
    static std::shared_ptr<User> take(int slot);

    /** Snapshot the registered users for safe iteration outside the registry. */
    static std::vector<std::shared_ptr<User>> snapshot();

    std::string getUsername() const {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        return username;
    }

    UserState getState() const {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        return state;
    }

    inline int getCommSlot() const {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        return comm_slot;
    }

    void closeComm() {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        if (comm_slot >= 0)
            comm_close(nullptr, comm_slot);
    }

    void addMessage(const std::string& message) {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        if (comm_slot >= 0)
            comm_add_message(comm_slot, message.data(), message.size());
    }

    /** Record activity received from this user's transport. */
    void resetIdleTime() {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        idle_time = std::chrono::steady_clock::now();
    }

    /** Return whether this connected user has been inactive for at least timeout. */
    bool isIdleFor(std::chrono::steady_clock::duration timeout) const {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        return comm_slot >= 0 && std::chrono::steady_clock::now() - idle_time > timeout;
    }

    void setCharInput () {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        if (comm_slot >= 0)
            comm_set_char_input (comm_slot);
    }

    void setLineInput (bool echo = true) {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        if (comm_slot >= 0)
            comm_set_line_input (comm_slot, echo);
    }

    void doMenu (std::unique_ptr<Menu> new_menu, void (User::* handler)(const std::string& message)) {
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        menu = std::move(new_menu);
        if (menu) {
            prompt_handler = &User::promptCurrentMenu;
            inbound_handler = handler;
            setCharInput(); // set the user to character input mode for menu selection
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
        std::lock_guard<std::recursive_mutex> lock(users_mutex);
        if (menu) {
            menu->writeMenu(shared_from_this());
        }
    }
};

#endif  // CHATROOM_USER_HPP
