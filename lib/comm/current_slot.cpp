#include "current_slot.hpp"

namespace {

/**
 * A thread local store for the slot number for slot-bounded hook functions.
 */
thread_local int current_slot = -1;

} // namespace

extern "C" int comm_current_slot(void) {
    return current_slot;
}

comm_current_slot_scope_t::comm_current_slot_scope_t(int slot)
    : previous_slot_(current_slot) {
    current_slot = slot;
}

comm_current_slot_scope_t::~comm_current_slot_scope_t() {
    current_slot = previous_slot_;
}
