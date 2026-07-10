#ifndef COMM_TELNET_HPP
#define COMM_TELNET_HPP

#include <type_traits>

#define S_TELNET_DATA       0x0
#define S_TELNET_IAC        0x1
#define S_TELNET_IAC_WILL   0x2
#define S_TELNET_IAC_WONT   0x3
#define S_TELNET_IAC_DO     0x4
#define S_TELNET_IAC_DONT   0x5

typedef struct comm_telnet_negotiation_s {
    uint32_t will_[8]; // bitmask of options agreed to enable
    uint32_t do_[8];   // bitmask of options requested to enable
    uint32_t wont_[8]; // bitmask of options agreed to disable
    uint32_t dont_[8]; // bitmask of options requested to disable
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
 * @param dest The destination buffer to copy non-Telnet data to.
 * @param src The source buffer containing data received from the Telnet client.
 * @param src_len The length of the source buffer.
 * @param state A pointer to the current Telnet negotiation state (S_TELNET_* constants).
 * @param negotiation A pointer to a comm_telnet_negotiation_t struct to track
 * the options negotiated with the client.
 * @return The number of bytes copied to the destination buffer (non-Telnet data).
 */
size_t comm_telnet_process_inbound (char* dest, char* src, size_t src_len,
    uint32_t* state, comm_telnet_negotiation_t* negotiation);

#endif // COMM_TELNET_HPP
