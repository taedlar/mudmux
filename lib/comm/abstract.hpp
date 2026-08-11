#ifndef COMM_ABSTRACT_HPP
#define COMM_ABSTRACT_HPP

#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "async/async_runtime.h"

typedef struct inbound_buffer_s inbound_buffer_t;
typedef struct outbound_buffer_s outbound_buffer_t;
typedef struct comm_websocket_state_s comm_websocket_state_t;

typedef struct comm_abstract_s {
    BIO *rbio; // could be null or equal to wbio for bidirectional sockets
    BIO *wbio; // could be null or equal to rbio for bidirectional sockets
    SSL *ssl; // could be null if SSL/TLS is not enabled for this comm
    inbound_buffer_t* inbound;
    outbound_buffer_t* outbound;
    outbound_buffer_t* websocket_upgrade_barrier;
    comm_websocket_state_t* websocket;
    uint64_t generation; // changes each time this numeric slot is assigned
    uint32_t flags;
    struct client_capabilities_s { // bitfields of client capabilities (negotiated via TELNET or other protocols)
        uint32_t telnet_linemode : 1; // client says WILL TELOPT_LINEMODE (RFC 1184)
        uint32_t telnet_naws : 1; // client says WILL TELOPT_NAWS (RFC 1073)
        uint32_t telnet_ttype : 1; // client says WILL TELOPT_TTYPE (RFC 930)
        uint32_t telnet_tspeed : 1; // client says WILL TELOPT_TSPEED (RFC 1079)
        uint32_t telnet_new_environ : 1; // client says WILL TELOPT_NEW_ENVIRON (RFC 1572)
        uint32_t telnet_echo : 1; // client says DO TELOPT_ECHO (RFC 857); server-side echo is negotiated
    } caps;
} comm_abstract_t;

static_assert(std::is_trivially_default_constructible_v<comm_abstract_t>,
    "comm_abstract_t must be trivially default constructible"); // for std::calloc to work correctly
static_assert(std::is_trivially_copyable_v<comm_abstract_t>,
    "comm_abstract_t must be trivially copyable"); // for std::realloc to work correctly

/* socket type BIO helpers */
#ifdef _WIN32
static inline socket_fd_t comm_bio_fd_to_socket_fd (int bio_fd) {
    return static_cast<socket_fd_t>(static_cast<unsigned int>(bio_fd));
}

static inline bool comm_socket_fd_to_bio_fd (socket_fd_t fd, int* out_bio_fd) {
    if (!out_bio_fd || fd == INVALID_SOCKET_FD)
        return false;

    if (fd > static_cast<socket_fd_t>((std::numeric_limits<int>::max)()))
        return false;

    *out_bio_fd = static_cast<int>(fd);
    return true;
}
#else
static inline socket_fd_t comm_bio_fd_to_socket_fd (int bio_fd) {
    return static_cast<socket_fd_t>(bio_fd);
}

static inline bool comm_socket_fd_to_bio_fd (socket_fd_t fd, int* out_bio_fd) {
    if (!out_bio_fd || fd == INVALID_SOCKET_FD)
        return false;

    *out_bio_fd = static_cast<int>(fd);
    return true;
}
#endif

static inline bool comm_bio_get_socket_fd (BIO* bio, socket_fd_t* out_fd) {
    if (!bio || !out_fd)
        return false;

    int bio_fd = -1;
    if (BIO_get_fd(bio, &bio_fd) < 0 || bio_fd < 0) {
        SPDLOG_ERROR ("BIO_get_fd() failed or returned invalid fd: {}", ERR_error_string(ERR_get_error(), nullptr));
        return false;
    }

    *out_fd = comm_bio_fd_to_socket_fd(bio_fd);
    return *out_fd != INVALID_SOCKET_FD;
}

/* comm_abstract_t life cycle */
int comm_max_slot (void);
int comm_abstract_add_bio (BIO *rbio, BIO *wbio, int slot, uint32_t flags);
int comm_abstract_add_file(const char *fn_in, const char* fn_out, int slot, uint32_t flags);
comm_abstract_t* comm_abstract_get (int slot);
int comm_abstract_remove (int slot);
uint64_t comm_abstract_generation(int slot);
void comm_abstract_remove_all (void);
bool comm_abstract_has_rbio (int slot);
bool comm_abstract_has_wbio (int slot);
bool comm_abstract_get_rbio_fd (int slot, socket_fd_t* out_fd);
bool comm_abstract_get_wbio_fd (int slot, socket_fd_t* out_fd);
extern std::recursive_mutex comm_slots_mtx;

/* flag management (logic layer helper) */
extern "C" uint32_t comm_get_flags (int slot);

/**
 * comm_abstract_ptr is a RAII wrapper for comm_abstract_t that locks the comm_slots_mtx
 * and provides safe access to the underlying comm_abstract_t for a given slot.
 */
class comm_abstract_ptr {
private:
    std::unique_lock<std::recursive_mutex> lock_;
    int slot_;
    static comm_abstract_t* all_comms_;
    static size_t max_comms_;

    static int ensure_capacity(size_t required_slots = 2);
    static int find_slot(void);
    static int max_slot_count(void);
    static void reset_storage(void);

    static inline comm_abstract_t* slot_ptr_unlocked(int slot) {
        if (!all_comms_ || slot < 0 || slot >= static_cast<int>(max_comms_))
            return nullptr;
        return &all_comms_[slot];
    }

    static inline comm_abstract_t* resolve_slot_unlocked(int slot) {
        comm_abstract_t* comm = slot_ptr_unlocked(slot);
        return (comm && (comm->rbio || comm->wbio)) ? comm : nullptr;
    }
public:
    comm_abstract_ptr(int slot, std::recursive_mutex& mtx) : lock_(mtx), slot_(slot) {
    }

    comm_abstract_ptr(const comm_abstract_ptr&) = delete;
    comm_abstract_ptr& operator=(const comm_abstract_ptr&) = delete;
    comm_abstract_ptr(comm_abstract_ptr&&) = default;
    comm_abstract_ptr& operator=(comm_abstract_ptr&&) = default;

    static inline comm_abstract_t* get_slot(int slot) { // comm_abstract_get() accessor to comm_abstract_t
        return resolve_slot_unlocked(slot);
    }

    inline comm_abstract_t* raw() const { // RAII accessor to slot pointer (could be unused slot)
        return slot_ptr_unlocked(slot_);
    }

    inline comm_abstract_t* get() const { // RAII accessor to valid slot pointer (returns nullptr if slot is unused)
        return resolve_slot_unlocked(slot_);
    }

    inline comm_abstract_t* operator->() const { // comm_abstract_ptr->member syntax, equivalent to get()
        return get();
    }

    inline explicit operator bool() const { // RAII validity check, returns true if slot is valid and not unused
        return resolve_slot_unlocked(slot_) != nullptr;
    }

    inline int slot() const { // immutable accessor to the slot number
        return slot_;
    }

    inline bool has_rbio() const { // RAII check if the slot has a valid read BIO
        auto* comm = resolve_slot_unlocked(slot_);
        return comm && comm->rbio;
    }

    inline bool has_wbio() const { // RAII check if the slot has a valid write BIO
        auto* comm = resolve_slot_unlocked(slot_);
        return comm && comm->wbio;
    }

    inline bool get_rbio_fd(socket_fd_t* out_fd) const { // RAII accessor to the read BIO's socket file descriptor
        auto* comm = resolve_slot_unlocked(slot_);
        return comm && comm->rbio && comm_bio_get_socket_fd(comm->rbio, out_fd);
    }

    inline bool get_wbio_fd(socket_fd_t* out_fd) const { // RAII accessor to the write BIO's socket file descriptor
        auto* comm = resolve_slot_unlocked(slot_);
        return comm && comm->wbio && comm_bio_get_socket_fd(comm->wbio, out_fd);
    }

    // other exposed methods that require access to private members
    friend int comm_max_slot (void);
    friend int comm_abstract_add_bio (BIO *rbio, BIO *wbio, int slot, uint32_t flags);
    friend void comm_abstract_remove_all (void);

}; // comm_abstract_ptr

#endif /* COMM_ABSTRACT_HPP */
