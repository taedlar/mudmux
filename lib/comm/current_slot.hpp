#ifndef COMM_CURRENT_SLOT_HPP
#define COMM_CURRENT_SLOT_HPP

#include "mudmux/hooks.h"

/**
 * Return the slot for the slot-scoped hook callback currently executing on
 * this thread, or -1 when no such callback is active.
 */
extern "C" int comm_current_slot(void);
enum mudmux_hook_type_t comm_current_hook_type(void);

class comm_current_slot_scope_t {
public:
    explicit comm_current_slot_scope_t(int slot);
    ~comm_current_slot_scope_t();

    comm_current_slot_scope_t(const comm_current_slot_scope_t&) = delete;
    comm_current_slot_scope_t& operator=(const comm_current_slot_scope_t&) = delete;

private:
    int previous_slot_;
};

class comm_hook_type_scope_t {
public:
    explicit comm_hook_type_scope_t(enum mudmux_hook_type_t hook_type);
    ~comm_hook_type_scope_t();
    comm_hook_type_scope_t(const comm_hook_type_scope_t&) = delete;
    comm_hook_type_scope_t& operator=(const comm_hook_type_scope_t&) = delete;
private:
    enum mudmux_hook_type_t previous_hook_type_;
};

#endif /* COMM_CURRENT_SLOT_HPP */
