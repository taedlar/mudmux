#include "current_slot.hpp"

namespace {

/**
 * A thread local store for the slot number for slot-bounded hook functions.
 */
thread_local int current_slot = -1;
thread_local mudmux_hook_type_t current_hook_type = MAX_HOOK_TYPE;

} // namespace

extern "C" int comm_current_slot(void) {
    return current_slot;
}

enum mudmux_hook_type_t comm_current_hook_type(void) { return current_hook_type; }

comm_current_slot_scope_t::comm_current_slot_scope_t(int slot)
    : previous_slot_(current_slot) {
    current_slot = slot;
}

comm_current_slot_scope_t::~comm_current_slot_scope_t() {
    current_slot = previous_slot_;
}

comm_hook_type_scope_t::comm_hook_type_scope_t(enum mudmux_hook_type_t hook_type)
    : previous_hook_type_(current_hook_type) { current_hook_type = hook_type; }

comm_hook_type_scope_t::~comm_hook_type_scope_t() { current_hook_type = previous_hook_type_; }
