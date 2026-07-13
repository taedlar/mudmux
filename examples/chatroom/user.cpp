#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <spdlog/spdlog.h>

#include "mudmux/comm.h"
#include "user.hpp"

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
    comm_abstract_t* comm = comm_abstract_get(comm_slot);
    if (!comm)
        return;
    *comm << "> "; // display prompt for user input
}

void User::dispatchInboundMessage(const std::string& message) {
    if (inbound_handler) {
        (this->*inbound_handler)(message); // call the inbound handler function
    } else {
        auto comm = comm_abstract_get(comm_slot);
        if (comm)
            *comm << fmt::format("Received message: [{}]\n\r", message);
        if (message == "quit" || message == "exit") {
            comm_close(nullptr, comm_slot); // close the connection on "quit" or "exit"
        }
    }
}

void User::receiveUsername(const std::string& name) {
    username = name;
    state = UserState::LoggedIn; // set state to LoggedIn after receiving the username
    inbound_handler = nullptr; // reset the inbound handler to nullptr since we no longer need it
    auto comm = comm_abstract_get(comm_slot);
    if (comm)
        *comm << fmt::format("Welcome, {}!\n\r", username);
}
