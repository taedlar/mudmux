#ifndef COMM_WEBSOCKET_HPP
#define COMM_WEBSOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "abstract.hpp"
#include "inbound.hpp"

/**
 * @brief Enable WebSocket upgrade handling for a communication slot.
 *
 * This sets a slot into HTTP upgrade mode. Incoming bytes are buffered until
 * a complete HTTP Upgrade request is received. On a valid RFC6455 handshake,
 * mudmux sends a "101 Switching Protocols" response and marks the slot as
 * WebSocket-ready.
 *
 * @param slot Communication slot.
 * @param preferred_protocols Comma-separated server subprotocol preferences, in
 * descending priority order. Pass null or an empty string to negotiate none.
 * @return true if WebSocket handling is enabled (or already enabled), false otherwise.
 */
extern "C" bool comm_enable_websocket(int slot, const char* preferred_protocols);

void comm_try_upgrade_websocket(async_runtime_t* runtime, comm_abstract_ptr& comm);

/**
 * @brief Build a successful WebSocket handshake response from an HTTP request.
 *
 * @param request Raw HTTP request bytes.
 * @param response Receives the 101 response when request is valid.
 * @param rejection_status Receives HTTP rejection status (e.g. 400, 426) when invalid.
 * @param preferred_protocols Comma-separated server preferences in descending priority order.
 * @param negotiated_telnet_subprotocol Set to true only when the selected protocol is
 *        "telnet.ietf.org" or "telnet.mudstandards.org".
 * @return true if request is a valid RFC6455 upgrade request, false otherwise.
 */
bool comm_websocket_build_upgrade_response(
    std::string_view request,
    std::string_view preferred_protocols,
    std::string& response,
    int* rejection_status = nullptr,
    bool* negotiated_telnet_subprotocol = nullptr);

/** Build an unmasked server-to-client RFC 6455 frame. */
bool comm_websocket_encode_frame(std::string_view payload, uint8_t opcode, std::string& frame);

/** Queue one server Close control frame, if one has not already been sent. */
bool comm_websocket_queue_close(comm_abstract_ptr& comm, std::string_view payload);

/**
 * Consume complete client WebSocket frames from @p wire and return complete text
 * messages.  Partial frames remain unconsumed for the next read.
 */
bool comm_websocket_process_inbound(
    comm_abstract_ptr& comm,
    std::string_view wire,
    size_t* consumed,
    std::vector<std::string>& messages,
    int* close_code = nullptr);

/** Release per-slot WebSocket fragmentation state. */
void comm_websocket_free_state(comm_abstract_ptr& comm);

/** Return the server subprotocol preference list configured for this slot. */
std::string_view comm_websocket_preferred_protocols(comm_abstract_ptr& comm);

comm_process_result_t comm_process_websocket_input(async_runtime_t* runtime, comm_abstract_ptr& comm,
                                                       int max_message, int& num_messages_processed);
#endif // COMM_WEBSOCKET_HPP
