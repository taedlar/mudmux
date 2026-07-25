#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "websocket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "abstract.hpp"
#include "outbound.hpp"
#include "mudmux/comm.h"

struct comm_websocket_state_s {
    uint8_t fragmented_opcode{0};
    std::string fragmented_payload;
    std::string preferred_protocols;
};

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

static std::string select_protocol(std::string_view preferred_protocols, std::string_view client_protocols) {
    size_t start = 0;
    while (start < preferred_protocols.size()) {
        const size_t comma = preferred_protocols.find(',', start);
        const std::string candidate = trim_ascii(preferred_protocols.substr(
            start, comma == std::string_view::npos ? preferred_protocols.size() - start : comma - start));
        if (!candidate.empty() && token_list_contains(client_protocols, to_lower_ascii(candidate)))
            return candidate;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return {};
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

static bool is_valid_utf8(std::string_view value) {
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[i++]);
        if (first <= 0x7f)
            continue;
        unsigned int codepoint = 0;
        size_t continuation_count = 0;
        if (first >= 0xc2 && first <= 0xdf) { codepoint = first & 0x1f; continuation_count = 1; }
        else if (first >= 0xe0 && first <= 0xef) { codepoint = first & 0x0f; continuation_count = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { codepoint = first & 0x07; continuation_count = 3; }
        else return false;
        if (i + continuation_count > value.size()) return false;
        for (size_t j = 0; j < continuation_count; ++j) {
            const unsigned char next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation_count == 1 && codepoint < 0x80) ||
            (continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
            return false;
    }
    return true;
}

bool comm_websocket_encode_frame(std::string_view payload, uint8_t opcode, std::string& frame) {
    if (opcode > 0x0f || payload.size() > static_cast<size_t>((std::numeric_limits<uint64_t>::max)()))
        return false;
    frame.clear();
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        const uint64_t size = static_cast<uint64_t>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((size >> shift) & 0xff));
    }
    frame.append(payload.data(), payload.size());
    return true;
}

void comm_websocket_free_state(comm_abstract_ptr& comm) {
    if (comm && comm->websocket) {
        delete comm->websocket;
        comm->websocket = nullptr;
    }
}

std::string_view comm_websocket_preferred_protocols(comm_abstract_ptr& comm) {
    return (comm && comm->websocket) ? std::string_view(comm->websocket->preferred_protocols) : std::string_view{};
}

bool comm_websocket_process_inbound(comm_abstract_ptr& comm, std::string_view wire, size_t* consumed,
                                    std::vector<std::string>& messages, int* close_code) {
    constexpr size_t kMaxMessageBytes = 64 * 1024;
    if (consumed) *consumed = 0;
    if (close_code) *close_code = 1002;
    if (!comm) return false;
    if (!comm->websocket) comm->websocket = new comm_websocket_state_t();
    comm_websocket_state_t& state = *comm->websocket;
    size_t offset = 0;
    while (offset < wire.size()) {
        if (wire.size() - offset < 2) break;
        const uint8_t first = static_cast<uint8_t>(wire[offset]);
        const uint8_t second = static_cast<uint8_t>(wire[offset + 1]);
        const bool fin = (first & 0x80) != 0;
        const uint8_t rsv = first & 0x70;
        const uint8_t opcode = first & 0x0f;
        const bool masked = (second & 0x80) != 0;
        uint64_t payload_size = second & 0x7f;
        size_t header_size = 2;
        if (rsv != 0 || !masked || (opcode >= 3 && opcode <= 7) || opcode >= 0x0b) return false;
        if (payload_size == 126) {
            if (wire.size() - offset < 4) break;
            payload_size = (static_cast<uint8_t>(wire[offset + 2]) << 8) | static_cast<uint8_t>(wire[offset + 3]);
            header_size += 2;
        } else if (payload_size == 127) {
            if (wire.size() - offset < 10) break;
            if (static_cast<uint8_t>(wire[offset + 2]) & 0x80) return false;
            payload_size = 0;
            for (size_t i = 0; i < 8; ++i) payload_size = (payload_size << 8) | static_cast<uint8_t>(wire[offset + 2 + i]);
            header_size += 8;
        }
        const bool control = (opcode & 0x08) != 0;
        if (wire.size() - offset < header_size + 4)
            break;
        if ((control && (!fin || payload_size > 125)) || payload_size > kMaxMessageBytes) {
            if (close_code) *close_code = 1009;
            return false;
        }
        const size_t frame_size = header_size + 4 + static_cast<size_t>(payload_size);
        if (wire.size() - offset < frame_size) break;
        const char* mask = wire.data() + offset + header_size;
        std::string payload(static_cast<size_t>(payload_size), '\0');
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = wire[offset + header_size + 4 + i] ^ mask[i % 4];
        offset += frame_size;

        if (opcode == 0x8) { // close
            if (payload.size() == 1) return false;
            std::string close_frame;
            if (comm_websocket_encode_frame(payload, 0x8, close_frame))
                comm_buffered_write_raw_comm(comm, close_frame.data(), close_frame.size());
            if (close_code) *close_code = 0; // normal peer-initiated close
            return false;
        }
        if (opcode == 0x9) {
            std::string pong;
            if (comm_websocket_encode_frame(payload, 0xA, pong))
                comm_buffered_write_raw_comm(comm, pong.data(), pong.size());
            continue;
        }
        if (opcode == 0xA) continue;
        if (opcode == 0x0) {
            if (state.fragmented_opcode == 0) return false;
            state.fragmented_payload += payload;
            if (state.fragmented_payload.size() > kMaxMessageBytes) { if (close_code) *close_code = 1009; return false; }
            if (fin) {
                if (state.fragmented_opcode == 0x1 && !is_valid_utf8(state.fragmented_payload)) {
                    if (close_code) *close_code = 1007;
                    return false;
                }
                messages.push_back(std::move(state.fragmented_payload));
                state.fragmented_payload.clear(); state.fragmented_opcode = 0;
            }
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (state.fragmented_opcode != 0) return false;
            if (fin) {
                if (opcode == 0x1 && !is_valid_utf8(payload)) { if (close_code) *close_code = 1007; return false; }
                messages.push_back(std::move(payload));
            } else {
                state.fragmented_opcode = opcode;
                state.fragmented_payload = std::move(payload);
            }
        }
    }
    if (consumed) *consumed = offset;
    return true;
}

extern "C" bool comm_enable_websocket(int slot, const char* preferred_protocols) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm || !comm->rbio || !comm->wbio) {
        return false;
    }

    if (comm->flags & C_ENABLE_TELNET) {
        SPDLOG_WARN("cannot enable WebSocket on slot {} while TELNET is enabled", slot);
        return false;
    }

    if (!comm->websocket)
        comm->websocket = new comm_websocket_state_t();
    comm->websocket->preferred_protocols = preferred_protocols ? preferred_protocols : "";
    comm->flags |= C_ENABLE_WEBSOCKET;
    SPDLOG_DEBUG("enabled WebSocket upgrade handling for comm slot {}", slot);
    return true;
}

bool comm_websocket_build_upgrade_response(
    std::string_view request,
    std::string_view preferred_protocols,
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

    const std::string selected_protocol = select_protocol(preferred_protocols, headers[4]);
    if (!selected_protocol.empty()) {
        const bool telnet_subprot = selected_protocol == "telnet.ietf.org" || selected_protocol == "telnet.mudstandards.org";
        if (negotiated_telnet_subprotocol)
            *negotiated_telnet_subprotocol = telnet_subprot;
        response += "Sec-WebSocket-Protocol: " + selected_protocol + "\r\n";
    }

    response += "\r\n";

    return true;
}
