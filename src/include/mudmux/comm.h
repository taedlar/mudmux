#ifndef MUDMUX_COMM_H
#define MUDMUX_COMM_H

#ifndef MUDMUX_NO_OPENSSL
#include <openssl/bio.h>
#endif

#include <stdarg.h>
#include <stdbool.h>

#include "mudmux_export.h"
#include "async.h"

/* Communication flags */
#define C_CLOSING                   (1u<<31)
#define C_SOCKET_LISTENING          (1u<<30)
#define C_SOCKET_READABLE           (1u<<29)
#define C_SOCKET_WRITABLE           (1u<<28)
#define C_LINE_INPUT                (1u<<27)
#define C_CLIENT_ECHO               (1u<<26)
#define C_ENABLE_TELNET             (1u<<25)
#define C_ENABLE_ANSI               (1u<<24)
#define C_ENABLE_WEBSOCKET          (1u<<23)
#define C_ENABLE_PROMPT             (1u<<22)

/* Internal communication states */
#define C_INVOKED_PROMPT            (1u<<13)
#define C_BUFFERED_WRITE            (1u<<12)
#define C_TLS_ESTABLISHED           (1u<<11)
#define C_DEFERRED_INBOUND          (1u<<10)
#define C_AWAITING_HOOK             (1u<<9)
#define C_DISCONNECT_PENDING        (1u<<8)
#define C_TRANSPORT_READY           (1u<<14)
#define M_WEBSOCKET_STATE           0x000000f0u     /* used by WEBSOCKET */
#define M_TELNET_STATE              0x0000000fu     /* used by TELNET */

/* WebSocket lifecycle states. C_ENABLE_WEBSOCKET is intentionally separate. */
#define WS_HANDSHAKE       0x00u
#define WS_READY           0x10u
#define WS_TELNET_PENDING  0x20u
#define WS_CLOSE_SENT      0x30u
#define WS_CLOSE_RECEIVED  0x40u

#define C_WEBSOCKET_STATE(flags)    ((flags) & M_WEBSOCKET_STATE)
#define C_WEBSOCKET_IS_READY(flags) (C_WEBSOCKET_STATE(flags) != WS_HANDSHAKE)
#define C_WEBSOCKET_SET_STATE(flags, state) \
    ((flags) = ((flags) & ~M_WEBSOCKET_STATE) | ((state) & M_WEBSOCKET_STATE))

typedef struct comm_abstract_s comm_abstract_t;

enum comm_slot_e {
    COMM_SLOT_CONSOLE = 0,
    RESERVED_SLOTS = 1
};

/**
 * @brief Struct containing function pointers for communication API.
 * 
 * Using this struct to provide runtime binding that allows the mudmux library
 * to be configurable for plugins or optional features without requiring
 * compile-time linking. The mudmux library will initialize this struct with
 * the appropriate function pointers during mudmux_init().
 */
typedef struct mudmux_comm_api_s {
    int (*max_slot)(void);
    /**
     * Return the slot currently being parsed for a parser-initiated callback,
     * or -1 for any other callback and outside hook dispatch.
     */
    int (*current_slot)(void);
#ifndef MUDMUX_NO_OPENSSL
    int (*add_bio)(BIO *rbio, BIO *wbio, int slot, uint32_t flags);
#else
    int (*add_bio)(void *rbio, void *wbio, int slot, uint32_t flags);
#endif
    int (*add_file)(const char *fn_in, const char* fn_out, int slot, uint32_t flags);
    uint32_t (*get_flags)(int slot);
    void (*enable_prompt)(int slot, bool enable);
    bool (*enable_virtual_terminal)(int slot);
    bool (*set_line_input)(int slot, bool echo);
    bool (*set_char_input)(int slot);
    bool (*set_echo)(int slot, bool echo);
    /**
      * Deliver a message to to_slot through HOOK_MESSAGE_OUTBOUND, or buffer it
      * directly to to_slot when no outbound hook is registered. The hook's msg
      * value is the destination slot.
     */
    void (*add_message)(int to_slot, const void *buf, size_t len);
    void (*add_formatted_message)(int to_slot, const char *fmt, ...);
    void (*buffered_write)(int slot, const void *buf, size_t len);
    bool (*close)(async_runtime_t* runtime, int slot);
    bool (*ssl_init)(const char* certificate_path, const char* private_key_path);
    void (*ssl_deinit)(void);
    /** Enable a transport protocol for the current HOOK_CONNECT slot only. */
    void (*enable_telnet)(void);
    bool (*enable_websocket)(const char* preferred_protocols);
    void (*enable_tls)(void);
} mudmux_comm_api_v1_t;

#if !defined(MUDMUX_STATIC_DEFINE) && !defined(mudmux_EXPORTS)
/* public interface */
#define comm_max_slot                   mudmux_comm_api_v1->max_slot
#define comm_current_slot               mudmux_comm_api_v1->current_slot
#define comm_abstract_add_bio           mudmux_comm_api_v1->add_bio
#define comm_abstract_add_file          mudmux_comm_api_v1->add_file
#define comm_get_flags                  mudmux_comm_api_v1->get_flags
#define comm_enable_prompt              mudmux_comm_api_v1->enable_prompt
#define comm_enable_virtual_terminal    mudmux_comm_api_v1->enable_virtual_terminal
#define comm_set_line_input             mudmux_comm_api_v1->set_line_input
#define comm_set_char_input             mudmux_comm_api_v1->set_char_input
#define comm_set_echo                   mudmux_comm_api_v1->set_echo
#define comm_add_message                mudmux_comm_api_v1->add_message
#define comm_add_formatted_message      mudmux_comm_api_v1->add_formatted_message
#define comm_buffered_write             mudmux_comm_api_v1->buffered_write
#define comm_close                      mudmux_comm_api_v1->close
#define comm_ssl_init                   mudmux_comm_api_v1->ssl_init
#define comm_ssl_deinit                 mudmux_comm_api_v1->ssl_deinit
#define comm_enable_telnet              mudmux_comm_api_v1->enable_telnet
#define comm_enable_websocket           mudmux_comm_api_v1->enable_websocket
#define comm_enable_tls                 mudmux_comm_api_v1->enable_tls
#endif

#ifdef __cplusplus

extern "C" {
#endif

MUDMUX_EXPORT extern mudmux_comm_api_v1_t* mudmux_comm_api_v1;

#ifdef __cplusplus
}

#ifndef comm_buffered_write
#include "comm/outbound.hpp"
#endif
#endif

#endif /* MUDMUX_COMM_H */
