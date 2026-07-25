#ifndef MUDMUX_EXECUTION_HPP
#define MUDMUX_EXECUTION_HPP

#include <cstddef>

#include "hooks.hpp"
#include "mudmux/hooks.h"

enum mudmux_determinism_mode_t {
    MUDMUX_DETERMINISM_STRICT = 0,
    MUDMUX_DETERMINISM_RELAXED = 1,
};

void mudmux_execution_configure(int thread_pool_size);
bool mudmux_execution_start();
void mudmux_execution_stop();

int mudmux_execution_thread_pool_size();
mudmux_determinism_mode_t mudmux_execution_mode();
const char* mudmux_execution_mode_name();
bool mudmux_execution_is_worker_thread();
mudmux_dispatch_result_t mudmux_execution_enqueue_hook(
    enum mudmux_hook_type_t hook_type,
    void* ctx,
    int slot,
    const void* data,
    size_t size,
    mudmux_hook_completion_t completion = nullptr,
    void* completion_context = nullptr);
mudmux_dispatch_result_t mudmux_execution_enqueue_telnet_subneg(void* ctx, int slot, int option, const void* data, size_t size);
bool mudmux_execution_retry_pending_telnet_subneg(int slot);
bool mudmux_execution_has_pending_telnet_subneg(int slot);
bool mudmux_execution_should_dispatch_async(enum mudmux_hook_type_t hook_type);

#endif
