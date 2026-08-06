#ifndef COMM_CURRENT_SLOT_HPP
#define COMM_CURRENT_SLOT_HPP

/**
 * Return the parser slot for the hook callback currently executing on this
 * thread, or -1 when the callback was not initiated by a transport parser.
 */
extern "C" int comm_current_slot(void);

class comm_current_slot_scope_t {
public:
    explicit comm_current_slot_scope_t(int slot);
    ~comm_current_slot_scope_t();

    comm_current_slot_scope_t(const comm_current_slot_scope_t&) = delete;
    comm_current_slot_scope_t& operator=(const comm_current_slot_scope_t&) = delete;

private:
    int previous_slot_;
};

#endif /* COMM_CURRENT_SLOT_HPP */
