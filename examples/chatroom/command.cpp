#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "command.hpp"
#include "user.hpp"

std::map<std::string, Command::CommandHandler> Command::command_map; // mapping of command verb to handler function

static void write_slot_text(int slot, const std::string& text) {
    if (slot < 0)
        return;
    comm_buffered_write(slot, text.c_str(), text.size());
}

static void command_quit (std::shared_ptr<User> user, const std::string& args);

void Command::initialize() {
    // Register commands here
    register_command("help", [](std::shared_ptr<User> user, const std::string& args) {
        (void)args; // suppress unused parameter warning
        const int slot = user->getCommSlot();
        write_slot_text(slot, "Available commands:\r\n");
        write_slot_text(slot, "/help - Show this help message\r\n");
        write_slot_text(slot, "/quit - Disconnect from the chatroom\r\n");
        write_slot_text(slot, "/exit - Disconnect from the chatroom\r\n");
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
