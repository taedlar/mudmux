#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <spdlog/spdlog.h>

#include "mudmux/comm.h"
#include "command.hpp"
#include "user.hpp"

std::recursive_mutex User::users_mutex;
std::vector<std::shared_ptr<User>> User::slots; // mapping of comm slots to User instances

std::shared_ptr<User> User::connect(int slot) {
    if (slot < 0)
        return nullptr;

    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (slot >= static_cast<int>(slots.size()))
        slots.resize(static_cast<size_t>(slot) + 32);
    auto user = std::make_shared<User>(slot);
    slots[static_cast<size_t>(slot)] = user;
    return user;
}

std::shared_ptr<User> User::find(int slot) {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (slot < 0 || slot >= static_cast<int>(slots.size()))
        return nullptr;
    return slots[static_cast<size_t>(slot)];
}

std::shared_ptr<User> User::take(int slot) {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (slot < 0 || slot >= static_cast<int>(slots.size()))
        return nullptr;
    auto user = std::move(slots[static_cast<size_t>(slot)]);
    return user;
}

std::vector<std::shared_ptr<User>> User::snapshot() {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    return slots;
}

static void write_slot_text(int slot, const std::string& text) {
    if (slot < 0)
        return;
    comm_buffered_write(slot, text.c_str(), text.size());
}

void User::logon() {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (comm_slot < 0)
        return;
    write_slot_text(comm_slot, "Please enter your username: ");
    state = UserState::PreLogin; // set state to PreLogin to indicate that the user is in the process of logging in
    inbound_handler = &User::receiveUsername; // set the inbound handler to receiveUsername to handle the next input as the username
}

void User::disconnect() {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    write_slot_text(comm_slot, "Bye!\r\n");
    state = UserState::Disconnected;
    comm_slot = -1; // mark the communication slot as invalid
}

void User::prompt() {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (prompt_handler) {
        (this->*prompt_handler)(); // call the prompt handler function
        return;
    }
    if (inbound_handler) {
        // If an inbound handler is set, we don't display the prompt to avoid confusion
        return;
    }
    if (comm_slot < 0)
        return;
    write_slot_text(comm_slot, "[" + username + "] "); // display prompt for user input
}

void User::dispatchInboundMessage(const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (inbound_handler) {
        (this->*inbound_handler)(message); // call the inbound handler function
    } else {
        if (comm_slot < 0 || message.empty())
            return;
        if (message.front() == '/') {
            // Handle slash commands
            if (!Command::find_and_execute(message.substr(1), shared_from_this())) { // remove the leading '/' before searching for the command
                write_slot_text(comm_slot, "Unknown slash command: " + message + "\r\n");
            }
        } else {
            // Handle regular chat messages
            write_slot_text(comm_slot, "You said: " + message + "\r\n"); // echo the message back to the user
            // Broadcast the message to all other connected users
            std::string msg = username + " says: " + message + "\r\n";
            for (const auto& user_ptr : User::snapshot()) {
                if (user_ptr && user_ptr->getState() == UserState::LoggedIn && user_ptr->comm_slot != comm_slot) {
                    SPDLOG_DEBUG ("writing message to slot {}", user_ptr->comm_slot);
                    comm_add_message(user_ptr->comm_slot, msg.data(), msg.size()); // broadcast the message to other users
                }
            }
        }
    }
}

void User::receiveUsername(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (name.length() < 3 || name.length() > 16) {
        write_slot_text(comm_slot, "Username must be between 3 and 16 characters. Please enter a valid username: ");
        return; // do not proceed if the username is invalid
    }
    username = name;
    state = UserState::LoggedIn; // set state to LoggedIn after receiving the username
    inbound_handler = nullptr; // reset the inbound handler to nullptr since we no longer need it
    write_slot_text(comm_slot, fmt::format("Welcome, {}!\r\n", username));
    write_slot_text(comm_slot, "You are now logged in. Type your messages to chat with others.\r\n");
    write_slot_text(comm_slot, "You can also use slash commands like /help, /quit, etc. to interact with the chatroom.\r\n");
}

void User::receiveExitConfirmation(const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(users_mutex);
    if (comm_slot < 0)
        return;
    if (menu) {
        menu->receiveCharInput(message); // process the menu input
        if (menu->getSelectedIndex() == 0) { // "Yes" option
            closeComm(); // close the communication slot
        }
        else if (menu->getSelectedIndex() == 1) { // "No" option
            menu = nullptr; // clear the menu
            prompt_handler = nullptr; // reset the prompt handler
            inbound_handler = nullptr; // reset the inbound handler
            comm_set_line_input(comm_slot, true); // set line input mode back to normal
        }
        else {
            comm_set_char_input(comm_slot); // re-arm character input mode to continue receiving input
            return;
        }
        addMessage("\r\x1B[J"); // clear the line and move cursor to the beginning
    }
}
