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

mudmux_dispatch_result_t mudmux_dispatch_hook_after(
    enum mudmux_hook_type_t hook_type,
    void* ctx,
    int msg,
    const void* data,
    size_t size,
    mudmux_hook_completion_t completion,
    void* completion_context,
    int current_slot_) {
    // The outbound hook's message identifies its destination slot. Make that
    // slot available to the callback just like other slot-scoped hooks.
    const int callback_current_slot = hook_type == HOOK_MESSAGE_OUTBOUND && current_slot_ < 0
        ? msg
        : current_slot_;

    if (!mudmux_execution_should_dispatch_async(hook_type)) {
        const mudmux_dispatch_result_t result = static_cast<mudmux_dispatch_result_t>(
            mudmux_invoke_registered_hook(hook_type, ctx, msg, const_cast<void*>(data), size, true, callback_current_slot) < 0
                ? MUDMUX_DISPATCH_ERROR
                : MUDMUX_DISPATCH_OK);
        if (completion)
            completion(completion_context, msg);
        (void)mudmux_execution_finalize_await(current_slot_);
        return result;
    }

    if (hook_type == HOOK_TELNET_SUBNEG) {
        return MUDMUX_DISPATCH_ERROR;
    }

    // Only parser-originated input is forbidden from queueing.  A hook/API
    // request targeting another slot is an explicit lifecycle/action request;
    // retain it in that target slot's bounded FIFO after the active hook.
    const bool allow_pending = hook_type != HOOK_MESSAGE_INBOUND;
    const int queue_slot = hook_type == HOOK_MESSAGE_OUTBOUND
        ? msg
        : -1;
    return mudmux_execution_enqueue_hook(
        hook_type, ctx, msg, data, size, completion, completion_context, allow_pending, queue_slot, callback_current_slot);
}
