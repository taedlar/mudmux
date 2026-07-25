#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "websocket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "abstract.hpp"
#include "mudmux/comm.h"

namespace {

constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static std::string to_lower_ascii(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

static std::string trim_ascii(std::string_view in) {
    size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin])) != 0) {
        ++begin;
    }

    size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
        --end;
    }

    return std::string(in.substr(begin, end - begin));
}

static bool token_list_contains(std::string_view csv, std::string_view needle_lower) {
    size_t start = 0;
    while (start < csv.size()) {
        size_t comma = csv.find(',', start);
        std::string token = trim_ascii(csv.substr(start, comma == std::string_view::npos ? csv.size() - start : comma - start));
        if (!token.empty() && to_lower_ascii(token) == needle_lower) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

static bool parse_http_headers(
    std::string_view request,
    std::string& request_line,
    std::array<std::string, 5>& header_values) {
    // header_values order:
    // 0=upgrade, 1=connection, 2=sec-websocket-key, 3=sec-websocket-version, 4=sec-websocket-protocol
    request_line.clear();
    for (auto& value : header_values) {
        value.clear();
    }

    size_t line_start = 0;
    bool is_first_line = true;
    while (line_start < request.size()) {
        size_t line_end = request.find("\n", line_start);
        if (line_end == std::string_view::npos) {
            line_end = request.size();
        }

        std::string_view line = request.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.empty()) {
            return !request_line.empty();
        }

        if (is_first_line) {
            request_line.assign(line.begin(), line.end());
            is_first_line = false;
        } else {
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos && colon > 0) {
                std::string name = to_lower_ascii(trim_ascii(line.substr(0, colon)));
                std::string value = trim_ascii(line.substr(colon + 1));
                if (name == "upgrade") {
                    header_values[0] = value;
                } else if (name == "connection") {
                    header_values[1] = value;
                } else if (name == "sec-websocket-key") {
                    header_values[2] = value;
                } else if (name == "sec-websocket-version") {
                    header_values[3] = value;
                } else if (name == "sec-websocket-protocol") {
                    header_values[4] = value;
                }
            }
        }

        if (line_end == request.size()) {
            break;
        }
        line_start = line_end + 1;
    }

    return false;
}

static bool build_websocket_accept(std::string_view client_key, std::string& accept_key) {
    std::string source;
    source.reserve(client_key.size() + std::strlen(kWebSocketGuid));
    source.append(client_key.begin(), client_key.end());
    source.append(kWebSocketGuid);

    unsigned char digest[SHA_DIGEST_LENGTH];
    if (!SHA1(reinterpret_cast<const unsigned char*>(source.data()), source.size(), digest)) {
        return false;
    }

    unsigned char b64[((SHA_DIGEST_LENGTH + 2) / 3) * 4 + 1] = {0};
    const int encoded_len = EVP_EncodeBlock(b64, digest, SHA_DIGEST_LENGTH);
    if (encoded_len <= 0) {
        return false;
    }

    accept_key.assign(reinterpret_cast<const char*>(b64), static_cast<size_t>(encoded_len));
    return true;
}

} // namespace

extern "C" bool comm_enable_websocket(int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm || !comm->rbio || !comm->wbio) {
        return false;
    }

    if (comm->flags & C_ENABLE_TELNET) {
        SPDLOG_WARN("cannot enable WebSocket on slot {} while TELNET is enabled", slot);
        return false;
    }

    comm->flags |= C_ENABLE_WEBSOCKET;
    SPDLOG_DEBUG("enabled WebSocket upgrade handling for comm slot {}", slot);
    return true;
}

bool comm_websocket_build_upgrade_response(
    std::string_view request,
    std::string& response,
    int* rejection_status,
    bool* negotiated_telnet_subprotocol) {
    if (rejection_status) {
        *rejection_status = 400;
    }
    response.clear();

    std::string request_line;
    std::array<std::string, 5> headers;
    if (!parse_http_headers(request, request_line, headers)) {
        return false;
    }

    if (request_line.rfind("GET ", 0) != 0 || request_line.find("HTTP/1.1") == std::string::npos) {
        return false;
    }

    const std::string upgrade = to_lower_ascii(headers[0]);
    if (upgrade != "websocket") {
        return false;
    }

    if (!token_list_contains(headers[1], "upgrade")) {
        return false;
    }

    if (headers[2].empty()) {
        return false;
    }

    if (!headers[3].empty() && trim_ascii(headers[3]) != "13") {
        if (rejection_status) {
            *rejection_status = 426;
        }
        return false;
    }

    std::string accept_key;
    if (!build_websocket_accept(headers[2], accept_key)) {
        return false;
    }

    response = "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + accept_key + "\r\n";

    if (!headers[4].empty()) {
        const bool telnet_subprot = token_list_contains(headers[4], "telnet");
        if (negotiated_telnet_subprotocol)
            *negotiated_telnet_subprotocol = telnet_subprot;
        if (telnet_subprot)
            response += "Sec-WebSocket-Protocol: telnet\r\n";
        else
            response += "Sec-WebSocket-Protocol: " + headers[4] + "\r\n";
    }

    response += "\r\n";

    return true;
}
