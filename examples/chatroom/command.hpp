#ifndef CHATROOM_COMMAND_HPP
#define CHATROOM_COMMAND_HPP

#include <map>
#include <memory>
#include <string>

class User;

class Command {
public:
    static void initialize();

    using CommandHandler = void (*)(std::shared_ptr<User> user, const std::string& args);
    static void register_command(const std::string& verb, CommandHandler handler) {
        command_map[verb] = handler;
    }

    static bool find_and_execute(const std::string& input, std::shared_ptr<User> user) {
        auto space_pos = input.find(' ');
        std::string verb = (space_pos == std::string::npos) ? input : input.substr(0, space_pos);
        std::string args = (space_pos == std::string::npos) ? "" : input.substr(space_pos + 1);

        auto it = command_map.find(verb);
        if (it != command_map.end()) {
            it->second(user, args); // call the handler function
            return true;
        }
        return false; // command not found
    }

private:
    static std::map<std::string, CommandHandler> command_map; // mapping of command verb to handler function
};

#endif  // CHATROOM_COMMAND_HPP
