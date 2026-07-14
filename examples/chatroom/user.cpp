#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <spdlog/spdlog.h>

#include "mudmux/comm.h"
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
            if (message == "/help") {
                *comm << "Available commands:\n\r";
                *comm << "/help - Show this help message\n\r";
                *comm << "/quit - Disconnect from the chatroom\n\r";
                *comm << "/exit - Disconnect from the chatroom\n\r";
            } else if (message == "/quit" || message == "/exit") {
                comm_close(nullptr, comm_slot); // close the connection on "quit" or "exit"
            } else {
                *comm << "Unknown command: " << message << "\n\r";
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
    username = name;
    state = UserState::LoggedIn; // set state to LoggedIn after receiving the username
    inbound_handler = nullptr; // reset the inbound handler to nullptr since we no longer need it
    auto comm = comm_abstract_get(comm_slot);
    if (comm) {
        *comm << fmt::format("Welcome, {}!\n\r", username);
        *comm << "You are now logged in. Type your messages to chat with others.\n\r";
        *comm << "You can also use slash commands like /help, /quit, etc. to interact with the chatroom.\n\r";
    }
}
