#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "command.hpp"
#include "user.hpp"

std::map<std::string, Command::CommandHandler> Command::command_map; // mapping of command verb to handler function

static void command_quit (std::shared_ptr<User> user, const std::string& args);

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

    register_command("quit", &command_quit);
    register_command("exit", &command_quit);
}

void command_quit (std::shared_ptr<User> user, const std::string& args) {
    (void)args; // suppress unused parameter warning
    std::unique_ptr<Menu> confirm_quit = std::make_unique<Menu>("Are you sure you want to quit? (y/n)");
    confirm_quit->addOption("Yes");
    confirm_quit->addOption("No");
    user->setCharInput(); // set the user to character input mode for menu selection
    user->doMenu(std::move(confirm_quit), &User::receiveExitConfirmation);
}
