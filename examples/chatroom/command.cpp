#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "command.hpp"
#include "user.hpp"

std::map<std::string, Command::CommandHandler> Command::command_map; // mapping of command verb to handler function

void Command::initialize() {
    // Register commands here
    register_command("help", [](std::shared_ptr<User> user, const std::string& args) {
        (void)args; // suppress unused parameter warning
        auto comm = user->getComm();
        if (comm) {
            *comm << "Available commands:\n\r";
            *comm << "/help - Show this help message\n\r";
            *comm << "/quit - Disconnect from the chatroom\n\r";
            *comm << "/exit - Disconnect from the chatroom\n\r";
        }
    });

    register_command("quit", [](std::shared_ptr<User> user, const std::string& args) {
        (void)args; // suppress unused parameter warning
        user->closeComm(); // close the connection on "quit"
    });

    register_command("exit", [](std::shared_ptr<User> user, const std::string& args) {
        (void)args; // suppress unused parameter warning
        user->closeComm(); // close the connection on "exit"
    });
}
