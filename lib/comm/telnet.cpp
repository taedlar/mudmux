#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "telnet.hpp"

#include "abstract.hpp"
#include "outbound.hpp"
#include "mudmux/comm.h"

void comm_enable_telnet (int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;
    if (comm->flags & C_ENABLE_TELNET)
        return;
    comm->flags |= C_ENABLE_TELNET;
    SPDLOG_DEBUG ("enabled TELNET for comm slot {}", slot);

    // Suppress obsolete Go Ahead (SGA), we are capable of full-duplex
    comm_telnet_send_will(comm, TELOPT_SGA);

    // We don't want to echo back what the client types (this will be used in password input)
    // Keeping WONT ECHO also enables Kludge line mode fallback for clients that don't support
    // LINEMODE (e.g., old tintin++, PuTTY, Windows Telnet).
    comm_telnet_send_wont(comm, TELOPT_ECHO);

    // Negotiate LINEMODE for clients that support it (RFC 1184). This is the preferred mode
    // for line input, as it allows the client to handle local echo and line editing.
    comm_telnet_send_do(comm, TELOPT_LINEMODE);
}

size_t comm_telnet_process_inbound (char* dest, char* src, size_t src_len, size_t* src_consumed,
    uint32_t* state, comm_telnet_negotiation_t* negotiation) {
    if (!dest || !src || !src_consumed || !state || !negotiation)
        return 0;

    negotiation->sb_len = 0; // reset subnegotiation length at the start of processing

    size_t dest_index = 0;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char byte = static_cast<unsigned char>(src[i]);
        switch (*state) {
            case S_TELNET_DATA:
                if (byte == 255) { // IAC
                    *state = S_TELNET_IAC;
                } else {
                    dest[dest_index++] = byte;
                }
                break;
            case S_TELNET_IAC:
                switch (byte) {
                    case 251: // WILL
                        *state = S_TELNET_IAC_WILL;
                        break;
                    case 252: // WONT
                        *state = S_TELNET_IAC_WONT;
                        break;
                    case 253: // DO
                        *state = S_TELNET_IAC_DO;
                        break;
                    case 254: // DONT
                        *state = S_TELNET_IAC_DONT;
                        break;
                    case 250: // SB (subnegotiation)
                        *state = S_TELNET_SUBNEG;
                        if (negotiation->sb_len) {
                            // 
                        }
                        break;
                    case 255: // IAC (escaped)
                        dest[dest_index++] = byte;
                        *state = S_TELNET_DATA;
                        break;
                    default:
                        *state = S_TELNET_DATA; // Unknown command, ignore
                        break;
                }
                break;
            case S_TELNET_IAC_WILL:
                negotiation->will_[byte >> 5] |= (1 << (byte & 31));
                negotiation->wont_[byte >> 5] &= ~(1 << (byte & 31)); // clear WONT if previously set
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: they WILL option {}", byte);
                break;
            case S_TELNET_IAC_WONT:
                negotiation->wont_[byte >> 5] |= (1 << (byte & 31));
                negotiation->will_[byte >> 5] &= ~(1 << (byte & 31)); // clear WILL if previously set
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: they WONT option {}", byte);
                break;
            case S_TELNET_IAC_DO:
                negotiation->do_[byte >> 5] |= (1 << (byte & 31));
                negotiation->dont_[byte >> 5] &= ~(1 << (byte & 31)); // clear DONT if previously set
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: please DO option {}", byte);
                break;
            case S_TELNET_IAC_DONT:
                negotiation->dont_[byte >> 5] |= (1 << (byte & 31));
                negotiation->do_[byte >> 5] &= ~(1 << (byte & 31)); // clear DO if previously set
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: please DONT option {}", byte);
                break;
            case S_TELNET_SUBNEG:
                if (byte == 255) { // IAC within subnegotiation
                    *state = S_TELNET_SUBNEG_IAC;
                } else {
                    if (negotiation->sb_len < sizeof(negotiation->subopt_buf)) {
                        negotiation->subopt_buf[negotiation->sb_len++] = byte; // buffer subnegotiation data
                    } else {
                        SPDLOG_WARN("subnegotiation buffer overflow, discarding byte {}", byte);
                    }
                }
                break;
            case S_TELNET_SUBNEG_IAC:
                if (byte == 240) { // SE (end of subnegotiation), return to caller to handle the subnegotiation data
                    *state = S_TELNET_DATA;
                    *src_consumed = i + 1; // include the SE byte in the consumed count
                    return dest_index; // return the number of bytes copied to dest
                } else if (byte == 255) { // escaped 0xFF (IAC IAC within subnegotiation)
                    if (negotiation->sb_len < sizeof(negotiation->subopt_buf)) {
                        negotiation->subopt_buf[negotiation->sb_len++] = static_cast<unsigned char>(255); // buffer IAC byte
                    } else {
                        SPDLOG_WARN("subnegotiation buffer overflow, discarding byte 255");
                    }
                    *state = S_TELNET_SUBNEG;
                } else {
                    // unidentified IAC sequence, treat as NOP and return to subnegotiation state
                    *state = S_TELNET_SUBNEG;
                }
                break;
            default:
                *state = S_TELNET_DATA; // Reset state on unexpected value
                break;
        }
    }
    *src_consumed = src_len; // all source bytes have been processed
    return dest_index;
}

void comm_telnet_send_will(comm_abstract_ptr& comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 251, static_cast<unsigned char>(option) }; // IAC WILL option
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(buf), sizeof(buf));
    SPDLOG_DEBUG("sent: we WILL option {}", option);
}

void comm_telnet_send_wont(comm_abstract_ptr& comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 252, static_cast<unsigned char>(option) }; // IAC WONT option
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(buf), sizeof(buf));
    SPDLOG_DEBUG("sent: we WONT option {}", option);
}

void comm_telnet_send_do(comm_abstract_ptr& comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 253, static_cast<unsigned char>(option) }; // IAC DO option
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(buf), sizeof(buf));
    SPDLOG_DEBUG("sent: please DO option {}", option);
}

void comm_telnet_send_dont(comm_abstract_ptr& comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 254, static_cast<unsigned char>(option) }; // IAC DONT option
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(buf), sizeof(buf));
    SPDLOG_DEBUG("sent: please DONT option {}", option);
}

void comm_telnet_send_subnegotiation(comm_abstract_ptr& comm, int option, const char* data, size_t len) {
    if (!comm || !comm->wbio || !data || len == 0)
        return;
    // Send IAC SB option ... IAC SE
    unsigned char iac_sb[3] = { 255, 250, static_cast<unsigned char>(option) }; // IAC SB option
    unsigned char iac_se[2] = { 255, 240 }; // IAC SE
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(iac_sb), sizeof(iac_sb));
    comm_buffered_write_comm(comm, data, len);
    comm_buffered_write_comm(comm, reinterpret_cast<char*>(iac_se), sizeof(iac_se));
    SPDLOG_DEBUG("sent: subnegotiation for option {} with {} bytes of data", option, len);
}
