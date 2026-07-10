#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "telnet.hpp"

#include "abstract.hpp"
#include "mudmux/comm.h"

void comm_enable_telnet (int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;
    if (comm->flags & C_ENABLE_TELNET)
        return;
    comm->flags |= C_ENABLE_TELNET;
    SPDLOG_DEBUG ("enabled Telnet negotiation for comm slot {}", slot);
    // TODO: initiate Telnet negotiation with the client (send IAC WILL/WONT DO/DONT sequences)
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
                SPDLOG_DEBUG("Telnet negotiation: WILL option {}", byte);
                break;
            case S_TELNET_IAC_WONT:
                negotiation->wont_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("Telnet negotiation: WONT option {}", byte);
                break;
            case S_TELNET_IAC_DO:
                negotiation->do_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("Telnet negotiation: DO option {}", byte);
                break;
            case S_TELNET_IAC_DONT:
                negotiation->dont_[byte >> 5] |= (1 << (byte & 31));
                *state = S_TELNET_DATA;
                SPDLOG_DEBUG("Telnet negotiation: DONT option {}", byte);
                break;
            default:
                *state = S_TELNET_DATA; // Reset state on unexpected value
                break;
        }
    }
    return dest_index;
}
