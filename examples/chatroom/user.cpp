#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <spdlog/spdlog.h>

#include "mudmux/comm.h"
#include "command.hpp"
#include "user.hpp"

std::vector<std::shared_ptr<User>> User::slots; // mapping of comm slots to User instances

void User::logon() {
    comm_abstract_t* comm = comm_abstract_get(comm_slot);
    if (!comm)
        return;
    *comm << "Please enter your username: ";
    state = UserState::PreLogin; // set state to PreLogin to indicate that the user is in the process of logging in
    inbound_handler = &User::receiveUsername; // set the inbound handler to receiveUsername to handle the next input as the username
}

void User::disconnect() {
    auto comm = comm_abstract_get(comm_slot);
    if (comm)
        *comm << "Bye!" << "\n\r";
    state = UserState::Disconnected;
    comm_slot = -1; // mark the communication slot as invalid
}

void User::prompt() {
    if (prompt_handler) {
        (this->*prompt_handler)(); // call the prompt handler function
        return;
    }
    if (inbound_handler) {
        // If an inbound handler is set, we don't display the prompt to avoid confusion
        return;
    }
    comm_abstract_t* comm = comm_abstract_get(comm_slot);
    if (!comm)
        return;
    *comm << "[" << username << "] "; // display prompt for user input
}

void User::dispatchInboundMessage(const std::string& message) {
    if (inbound_handler) {
        (this->*inbound_handler)(message); // call the inbound handler function
    } else {
        auto comm = comm_abstract_get(comm_slot);
        if (!comm)
            return;
        if (message.front() == '/') {
            // Handle slash commands
            if (!Command::find_and_execute(message.substr(1), shared_from_this())) { // remove the leading '/' before searching for the command
                *comm << "Unknown slash command: " << message << "\n\r";
            }
        } else {
            // Handle regular chat messages
            *comm << "You said: " << message << "\n\r"; // echo the message back to the user
            // Broadcast the message to all other connected users
            for (const auto& user_ptr : User::slots) {
                if (user_ptr && user_ptr->getState() == UserState::LoggedIn && user_ptr->comm_slot != comm_slot) {
                    auto other_comm = user_ptr->getComm();
                    if (other_comm) {
                        *other_comm << username << " says: " << message << "\n\r"; // broadcast the message to other users
                    }
                }
            }
        }
    }
}

void User::receiveUsername(const std::string& name) {
    auto comm = comm_abstract_get(comm_slot);
    if (name.length() < 3 || name.length() > 16) {
        if (comm) {
            *comm << "Username must be between 3 and 16 characters. Please enter a valid username: ";
        }
        return; // do not proceed if the username is invalid
    }
    username = name;
    state = UserState::LoggedIn; // set state to LoggedIn after receiving the username
    inbound_handler = nullptr; // reset the inbound handler to nullptr since we no longer need it
    if (comm) {
        *comm << fmt::format("Welcome, {}!\n\r", username);
        *comm << "You are now logged in. Type your messages to chat with others.\n\r";
        *comm << "You can also use slash commands like /help, /quit, etc. to interact with the chatroom.\n\r";
    }
}

void User::receiveExitConfirmation(const std::string& message) {
    auto comm = comm_abstract_get(comm_slot);
    if (!comm)
        return;
    if (menu) {
        menu->receiveCharInput(message); // process the menu input
        if (menu->getSelectedIndex() == 0) { // "Yes" option
            closeComm(); // close the communication slot
        }
        else if (menu->getSelectedIndex() == 1) { // "No" option
            menu.reset(); // clear the menu
            prompt_handler = nullptr; // reset the prompt handler
            inbound_handler = nullptr; // reset the inbound handler
        }
    }
}
