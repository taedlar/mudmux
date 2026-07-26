#ifndef MUDMUX_COMM_H
#define MUDMUX_COMM_H

#ifndef MUDMUX_NO_OPENSSL
#include <openssl/bio.h>
#endif

#include "mudmux_export.h"
#include "async.h"

#define C_CLOSING           0x80000000
#define C_SOCKET_LISTENING  0x40000000
#define C_SOCKET_READABLE   0x20000000
#define C_SOCKET_WRITABLE   0x10000000
#define C_LINE_INPUT        0x08000000
#define C_CLIENT_ECHO       0x04000000
#define C_ENABLE_TELNET     0x02000000
#define C_ENABLE_ANSI       0x01000000
#define C_ENABLE_WEBSOCKET  0x00800000
#define C_WEBSOCKET_READY   0x00400000
#define C_WEBSOCKET_TELNET_PENDING 0x00200000
#define C_WEBSOCKET_CLOSE_SENT     0x00100000
#define C_WEBSOCKET_CLOSE_RECEIVED 0x00080000
#define C_ENABLE_PROMPT     0x00040000
#define C_INVOKED_PROMPT    0x00020000
#define C_BUFFERED_WRITE    0x00010000
#define C_TLS_ESTABLISHED   0x00008000
#define M_TELNET_STATE      0x0000000f      /* used by TELNET */

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
#ifndef MUDMUX_NO_OPENSSL
    int (*add_bio)(BIO *rbio, BIO *wbio, int slot, uint32_t flags);
#else
    int (*add_bio)(void *rbio, void *wbio, int slot, uint32_t flags);
#endif
    int (*add_file)(const char *fn_in, const char* fn_out, int slot, uint32_t flags);
    uint32_t (*get_flags)(int slot);
    void (*buffered_write)(int slot, const void *buf, size_t len);
    bool (*close)(async_runtime_t* runtime, int slot);
    bool (*set_line_input)(int slot, bool echo);
    bool (*set_char_input)(int slot);
    bool (*set_echo)(int slot, bool echo);
    bool (*ssl_init)(const char* certificate_path, const char* private_key_path);
    void (*ssl_deinit)(void);
    void (*enable_telnet)(int slot);
    bool (*enable_websocket)(int slot, const char* preferred_protocols);
    void (*enable_prompt)(int slot, bool enable);
    bool (*enable_virtual_terminal)(int slot);
    void (*enable_tls)(int slot);
} mudmux_comm_api_v1_t;

#if !defined(MUDMUX_STATIC_DEFINE) && !defined(mudmux_EXPORTS)
/* public interface */
#define comm_max_slot                   mudmux_comm_api_v1->max_slot
#define comm_abstract_add_bio           mudmux_comm_api_v1->add_bio
#define comm_abstract_add_file          mudmux_comm_api_v1->add_file
#define comm_get_flags                  mudmux_comm_api_v1->get_flags
#define comm_buffered_write             mudmux_comm_api_v1->buffered_write
#define comm_close                      mudmux_comm_api_v1->close
#define comm_set_line_input             mudmux_comm_api_v1->set_line_input
#define comm_set_char_input             mudmux_comm_api_v1->set_char_input
#define comm_set_echo                   mudmux_comm_api_v1->set_echo
#define comm_ssl_init                   mudmux_comm_api_v1->ssl_init
#define comm_ssl_deinit                 mudmux_comm_api_v1->ssl_deinit
#define comm_enable_prompt              mudmux_comm_api_v1->enable_prompt
#define comm_enable_telnet              mudmux_comm_api_v1->enable_telnet
#define comm_enable_websocket           mudmux_comm_api_v1->enable_websocket
#define comm_enable_virtual_terminal    mudmux_comm_api_v1->enable_virtual_terminal
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
