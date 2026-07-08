#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "inbound.hpp"

#include "abstract.hpp"
#include "mudmux/hooks.h"

int comm_invoke_connect (async_runtime_t* runtime, int slot) {
    if (!runtime || slot < 0) {
        return -1;
    }

    return mudmux_invoke_hook (
        MUDMUX_HOOK_CONNECT,
        async_runtime_get_context(runtime),
        slot,
        nullptr,
        0
	);
}

int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size) {
    if (!runtime || !data || size == 0) {
        return -1;
    }

    mudmux_invoke_hook (
        MUDMUX_HOOK_MESSAGE_INBOUND,
        async_runtime_get_context(runtime),
        slot,
        const_cast<void*>(data),
        size);
    return 0;
}

int comm_process_input (
    async_runtime_t* runtime,
    const io_event_t* event,
    int slot) {
    if (!runtime || !event || slot < 0) {
        return -1;
    }
    comm_abstract_ptr comm(slot, mud_logic_mutex);
    if (!comm) {
        return -1;
    }

#ifdef _WIN32
    if (!event->buffer || event->bytes_transferred == 0) {
        return 1;
    }

    if (comm_invoke_inbound_message(runtime, slot, event->buffer, event->bytes_transferred) < 0) {
        return 1;
    }

    // inbound message hook function could have closed the connection ...
    if (!comm) {
        return 1;
    }

    if (async_runtime_post_read(runtime, event->fd, nullptr, 0) < 0) {
        SPDLOG_ERROR ("failed to re-arm read for fd {}", event->fd);
        return 1;
    }
#else
    char buffer[4096];
    int read_bytes = comm.read (buffer, sizeof(buffer));
    if (read_bytes <= 0) {
        return 1;
    }

    if (comm_invoke_inbound_message(runtime, slot, buffer, static_cast<size_t>(read_bytes)) < 0) {
        return 1;
    }

    if (event->event_type & (EVENT_CLOSE | EVENT_ERROR)) {
        return 1;
    }
#endif

    return 0;
}
