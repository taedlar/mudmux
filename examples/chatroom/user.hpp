#ifndef CHATROOM_USER_HPP
#define CHATROOM_USER_HPP

#include <memory>
#include <string>
#include <vector>

enum class UserState {
    Disconnected,
    Connected,
    PreLogin,
    LoggedIn
};

class User {
private:
    std::string username;
    int comm_slot;
    UserState state;
    void (User::* inbound_handler)(const std::string& message) = nullptr;

public:
    static std::vector<std::shared_ptr<User>> slots; // mapping of comm slots to User instances

    User(int slot) : comm_slot(slot), state(UserState::Disconnected) {}

    const std::string& getUsername() const {
        return username;
    }

    UserState getState() const {
        return state;
    }

    inline comm_abstract_t* getComm() const {
        return comm_abstract_get(comm_slot);
    }

    void logon();
    void disconnect();
    void prompt();
    void dispatchInboundMessage(const std::string& message);

    void receiveUsername (const std::string& name);
};

#endif  // CHATROOM_USER_HPP
