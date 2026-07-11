#ifndef COMM_TELNET_HPP
#define COMM_TELNET_HPP

#include <type_traits>
#ifdef HAVE_ARPA_TELNET_H
#include <arpa/telnet.h>
#else
#define TELOPT_BINARY 0
#define TELOPT_ECHO 1
#define TELOPT_SGA 3
#define TELOPT_TTYPE 24
#define TELOPT_NAWS 31
#define TELOPT_TSPEED 32
#define TELOPT_LINEMODE 34
#define TELOPT_NEW_ENVIRON 39
#endif

#include "abstract.hpp"

// telnet negotiation state-machine states
#define S_TELNET_DATA       0x0
#define S_TELNET_IAC        0x1
#define S_TELNET_IAC_WILL   0x2
#define S_TELNET_IAC_WONT   0x3
#define S_TELNET_IAC_DO     0x4
#define S_TELNET_IAC_DONT   0x5
#define S_TELNET_SUBNEG     0x6
#define S_TELNET_SUBNEG_IAC 0x7

typedef struct comm_telnet_negotiation_s {
    uint32_t will_[8]; // bitmask of options agreed to enable
    uint32_t do_[8];   // bitmask of options requested to enable
    uint32_t wont_[8]; // bitmask of options agreed to disable
    uint32_t dont_[8]; // bitmask of options requested to disable
    size_t sb_len;        // length of subnegotiation data in subopt_buf
    char subopt_buf[1024];  // buffer for subnegotiation data
} comm_telnet_negotiation_t;

static_assert(std::is_trivially_default_constructible_v<comm_telnet_negotiation_t>,
    "comm_telnet_negotiation_t must be trivially default constructible"); // for std::calloc to work correctly
static_assert(std::is_trivially_copyable_v<comm_telnet_negotiation_t>,
    "comm_telnet_negotiation_t must be trivially copyable");

/**
 * @brief Enable Telnet protocol support for a communication slot.
 * When this function is called on a slot without C_ENABLE_TELNET set, it will
 * initiate Telnet negotiation with the client and set the C_ENABLE_TELNET flag
 * in the slot's flags. The transport layer will then handle Telnet protocol
 * details for that slot, such as interpreting Telnet commands and options as
 * long as the C_ENABLE_TELNET flag remains set. This function is typically called
 * during the initial setup of a communication slot or in the connect hook.
 * 
 * If the slot already has C_ENABLE_TELNET set, this function will have no effect.
 * 
 * C_ENABLE_TELNET cannot be disabled once it has been enabled for a slot.
 * 
 * @param slot The communication slot to enable Telnet for.
 */
void comm_enable_telnet (int slot);

/**
 * @brief Process inbound data from a Telnet client, handling Telnet commands and options.
 * This function takes a source buffer of data received from a Telnet client and processes
 * any Telnet commands (IAC sequences) found in the data. It updates the negotiation
 * state and negotiation struct accordingly, and copies any non-Telnet data to the destination
 * buffer.
 *
 * If there is a Telnet subnegotiation in progress, the function will buffer the subnegotiation
 * data until the end of the subnegotiation is reached (IAC SE). At most one subnegotiation can
 * be in progress at a time. The function returns the number of bytes copied to the destination
 * buffer, which will contain only non-Telnet data.
 *
 * @param dest The destination buffer to copy non-Telnet data to.
 * @param src The source buffer containing data received from the Telnet client.
 * @param src_len The length of the source buffer.
 * @param src_consumed A pointer to a size_t to receive the number of bytes consumed from the source buffer.
 * @param state A pointer to the current Telnet negotiation state (S_TELNET_* constants).
 * @param negotiation A pointer to a comm_telnet_negotiation_t struct to track
 * the options negotiated with the client.
 * @return The number of bytes copied to the destination buffer (non-Telnet data).
 */
size_t comm_telnet_process_inbound (char* dest, char* src, size_t src_len, size_t* src_consumed,
    uint32_t* state, comm_telnet_negotiation_t* negotiation);

void comm_telnet_send_will(comm_abstract_t* comm, int option);
void comm_telnet_send_wont(comm_abstract_t* comm, int option);
void comm_telnet_send_do(comm_abstract_t* comm, int option);
void comm_telnet_send_dont(comm_abstract_t* comm, int option);

#endif // COMM_TELNET_HPP
