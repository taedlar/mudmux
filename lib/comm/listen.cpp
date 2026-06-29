#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async/socket_intf.h"
#include "listen.h"

/**
 *  @brief Initialize new user connection socket.
 */
extern "C" bool comm_init_listening_port (async_runtime_t *runtime, int port, void *ctx) {

    if (!runtime) {
        SPDLOG_ERROR ("Invalid async runtime pointer");
        return false;
    }

    if (port < 1 || port > 65535) {
        SPDLOG_ERROR ("Invalid port number: {}", port);
        return false;
    }

    socket_fd_t sock_fd = socket (AF_INET, SOCK_STREAM, 0);
    if (sock_fd == INVALID_SOCKET_FD) {
        SPDLOG_ERROR ("socket() failed: {}", SOCKET_ERRNO);
        return false;
    }

    int optval = 1;
    if (setsockopt (sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &optval, sizeof (optval)) == SOCKET_ERROR) {
        SPDLOG_ERROR ("setsockopt() failed: {}", SOCKET_ERRNO);
        SOCKET_CLOSE (sock_fd);
        return false;
    }

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons ((uint16_t) port);
    if (bind (sock_fd, (struct sockaddr *) &sin, sizeof (sin)) == SOCKET_ERROR) {
        SPDLOG_ERROR ("bind() failed: {}", SOCKET_ERRNO);
        SOCKET_CLOSE (sock_fd);
        return false;
    }

    socklen_t sin_len = sizeof (sin);
    if (getsockname (sock_fd, (struct sockaddr *) &sin, &sin_len) == SOCKET_ERROR) {
          SPDLOG_ERROR ("getsockname() failed: {}", SOCKET_ERRNO);
          SOCKET_CLOSE (sock_fd);
          return false;
    }

    if (set_socket_nonblocking (sock_fd, 1) == SOCKET_ERROR) {
        SPDLOG_ERROR ("set_socket_nonblocking() failed: {}", SOCKET_ERRNO);
        SOCKET_CLOSE (sock_fd);
        return false;
    }

    if (listen (sock_fd, SOMAXCONN) == SOCKET_ERROR) {
        SPDLOG_ERROR ("listen() failed: {}", SOCKET_ERRNO);
        SOCKET_CLOSE (sock_fd);
        return false;
    }
 
    if (async_runtime_add (runtime, sock_fd, EVENT_READ, ctx) != 0) {
        SPDLOG_ERROR ("async_runtime_add() failed for listening socket: {}", SOCKET_ERRNO);
        SOCKET_CLOSE (sock_fd);
        return false;
    }

#if 0
  {
    addr_resolver_config_t resolver_config;

    stem_get_addr_resolver_config (&resolver_config);
    if (!addr_resolver_init (g_runtime, &resolver_config))
      {
        debug_message ("Warning: resolver worker disabled; resolve/query_ip_name async refresh unavailable.\n");
      }
  }

#ifdef HAVE_CURL
  init_curl_subsystem ();
#endif

#ifndef _WIN32
  /* register signal handler for SIGPIPE. */
  if (signal (SIGPIPE, sigpipe_handler) == SIG_ERR)
    {
      debug_perror ("signal()", 0);
      debug_fatal ("Failed to set signal handler for SIGPIPE\n");
      exit (5);
    }
#endif
  
  add_ip_entry (INADDR_LOOPBACK, "localhost");
#endif

    SPDLOG_INFO ("Listening on port {}", port);
    return true;
}
