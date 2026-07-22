#ifndef COMM_WEBSOCKET_HPP
#define COMM_WEBSOCKET_HPP

#include <cstddef>
#include <string>
#include <string_view>

/**
 * @brief Enable WebSocket upgrade handling for a communication slot.
 *
 * This sets a slot into HTTP upgrade mode. Incoming bytes are buffered until
 * a complete HTTP Upgrade request is received. On a valid RFC6455 handshake,
 * mudmux sends a "101 Switching Protocols" response and marks the slot as
 * WebSocket-ready.
 *
 * @param slot Communication slot.
 * @return true if WebSocket handling is enabled (or already enabled), false otherwise.
 */
extern "C" bool comm_enable_websocket(int slot);

/**
 * @brief Build a successful WebSocket handshake response from an HTTP request.
 *
 * @param request Raw HTTP request bytes.
 * @param response Receives the 101 response when request is valid.
 * @param rejection_status Receives HTTP rejection status (e.g. 400, 426) when invalid.
 * @return true if request is a valid RFC6455 upgrade request, false otherwise.
 */
bool comm_websocket_build_upgrade_response(
    std::string_view request,
    std::string& response,
    int* rejection_status = nullptr);

#endif // COMM_WEBSOCKET_HPP
