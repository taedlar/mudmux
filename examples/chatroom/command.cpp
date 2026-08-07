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
        const int slot = user->getCommSlot();
        std::string help_message =
            "Welcome to the chatroom! Here are some commands you can use:\n"
            "/help - Show this help message\n"
            "/quit - Disconnect from the chatroom\n";
        comm_buffered_write(slot, help_message.c_str(), help_message.size());
    });

    register_command("quit", &command_quit);
}

void command_quit (std::shared_ptr<User> user, const std::string& args) {
    (void)args; // suppress unused parameter warning
    std::unique_ptr<Menu> confirm_quit = std::make_unique<Menu>("Are you sure you want to quit? (y/n)");
    confirm_quit->addOption("Yes");
    confirm_quit->addOption("No");
    user->doMenu(std::move(confirm_quit), &User::receiveExitConfirmation);
}
