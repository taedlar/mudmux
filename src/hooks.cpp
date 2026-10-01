#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include "execution.hpp"
#include "mudmux/mudmux.h"
#include "mudmux/hooks.h"
#include "comm/outbound.hpp"
#include "comm/current_slot.hpp"
#include "hooks.hpp"

bool spdlog_initialized{false};

static mudmux_hook_func_t all_hooks[MAX_HOOK_TYPE] = {nullptr}; // array of hook functions

mudmux_hook_func_t mudmux_get_registered_hook(enum mudmux_hook_type_t hook_type) {
    return hook_type > 0 && hook_type < MAX_PUBLIC_HOOKS ? all_hooks[hook_type] : nullptr;
}

int mudmux_invoke_hook(mudmux_hook_func_t hook_func, void* ctx, int msg, void* data, size_t size,
                                bool flush_after, int current_slot_) {
    int ret = 0;
    comm_current_slot_scope_t current_slot_scope(current_slot_);
    if (mudmux_execution_mode() == MUDMUX_DETERMINISM_STRICT) {
        std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
        ret = hook_func(ctx, msg, data, size);
    }
    else {
        ret = hook_func(ctx, msg, data, size);
    }

    if (flush_after)
        comm_flush_all(async_get_current_runtime());
    return ret;
}

int mudmux_invoke_registered_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size,
                                  bool flush_after, int current_slot_) {
    if (hook_type <= 0 || hook_type >= MAX_PUBLIC_HOOKS) {
        SPDLOG_ERROR ("invalid hook type for registered-hook dispatch");
        return -1;
    }

    mudmux_hook_func_t hook_func = all_hooks[hook_type];
    if (!hook_func)
        return 0;

    comm_hook_type_scope_t hook_type_scope(hook_type);
    return mudmux_invoke_hook(hook_func, ctx, msg, data, size, flush_after, current_slot_);
}

void mudmux_reset_registered_hooks() {
    for (int i = 0; i < MAX_HOOK_TYPE; ++i) {
        all_hooks[i] = nullptr;
    }
}

MUDMUX_EXPORT bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func) {
    if (hook_type <= 0 || hook_type >= MAX_PUBLIC_HOOKS || !hook_func) {
        SPDLOG_ERROR ("mudmux_register_hook() called with invalid hook_type or null hook_func");
        return false;
    }
    all_hooks[hook_type] = hook_func;
    return true;
}

MUDMUX_EXPORT void mudmux_register_logger_callback(mudmux_logger_callback_t callback, void* ctx) {
    if (!callback)
        return;

    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [callback, ctx](const spdlog::details::log_msg& log_msg) {
            const std::string message(log_msg.payload.data(), log_msg.payload.size());
            const char* file = log_msg.source.filename ? strstr(log_msg.source.filename, "mudmux") : "";
            callback(
                ctx,
                static_cast<int>(log_msg.level),
                file ? file : "",
                static_cast<int>(log_msg.source.line),
                log_msg.source.funcname ? log_msg.source.funcname : "",
                message.c_str());
        });

    spdlog::set_default_logger(std::make_shared<spdlog::logger>("mudmux", sink));
    spdlog_initialized = true;
}
