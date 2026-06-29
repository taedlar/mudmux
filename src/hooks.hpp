#ifndef MUDMUX_HOOKS_HPP
#define MUDMUX_HOOKS_HPP

#include "mudmux.h"

struct mudmux_hooks_t {
    mudmux_hook_func_t hook_connect;
    mudmux_hook_func_t hook_disconnect;
    mudmux_hook_func_t hook_message;
    mudmux_hook_func_t hook_input;
};

#endif // MUDMUX_HOOKS_HPP
