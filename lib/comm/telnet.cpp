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
    // comm_telnet_send_will(comm.raw(), TELOPT_BINARY);
    comm_telnet_send_will(comm.raw(), TELOPT_SGA);
    comm_telnet_send_do(comm.raw(), TELOPT_SGA);
    comm_telnet_send_wont(comm.raw(), TELOPT_ECHO);
}

size_t comm_telnet_process_inbound (char* dest, char* src, size_t src_len,
    uint32_t* state, comm_telnet_negotiation_t* negotiation) {
    if (!dest || !src || !state || !negotiation)
        return 0;

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
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: they WILL option {}", byte);
                break;
            case S_TELNET_IAC_WONT:
                negotiation->wont_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: they WONT option {}", byte);
                break;
            case S_TELNET_IAC_DO:
                negotiation->do_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: please DO option {}", byte);
                break;
            case S_TELNET_IAC_DONT:
                negotiation->dont_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("received: please DONT option {}", byte);
                break;
            default:
                *state = S_TELNET_DATA; // Reset state on unexpected value
                break;
        }
    }
    return dest_index;
}

void comm_telnet_send_will(comm_abstract_t* comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 251, static_cast<unsigned char>(option) }; // IAC WILL option
    comm_buffered_write(comm, reinterpret_cast<char*>(buf), sizeof(buf));
}

void comm_telnet_send_wont(comm_abstract_t* comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 252, static_cast<unsigned char>(option) }; // IAC WONT option
    comm_buffered_write(comm, reinterpret_cast<char*>(buf), sizeof(buf));
}

void comm_telnet_send_do(comm_abstract_t* comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 253, static_cast<unsigned char>(option) }; // IAC DO option
    comm_buffered_write(comm, reinterpret_cast<char*>(buf), sizeof(buf));
}

void comm_telnet_send_dont(comm_abstract_t* comm, int option) {
    if (!comm || !comm->wbio)
        return;
    unsigned char buf[3] = { 255, 254, static_cast<unsigned char>(option) }; // IAC DONT option
    comm_buffered_write(comm, reinterpret_cast<char*>(buf), sizeof(buf));
}
