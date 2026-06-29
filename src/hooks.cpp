#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux.h"

extern "C" bool mudmux_register_hook (const char* hook_name, mudmux_hook_func_t hook_func) {
    if (!hook_name || !hook_func) {
        SPDLOG_ERROR ("mudmux_register_hook() called with null hook_name or hook_func");
        return false;
    }
    return true; // TODO: implement hook registration
}
