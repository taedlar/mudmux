// main.cpp
#include "mudmux/mudmux.h"
#include "comm/abstract.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>

static void process_command_line (int argc, char* argv[]);

static int on_connect (void*, int slot, void*, size_t) {
    auto comm = comm_abstract_get(slot);
    comm_write (comm, "Welcome to mudmux!\r\n", 0);
    return 0;
}

static int on_message_inbound (void*, int slot, void* data, size_t size) {
    std::string message(static_cast<char*>(data), size);
    auto comm = comm_abstract_get(slot);
    comm_write (comm, "Received message: ", 0);
    comm_write (comm, message.c_str(), message.size());
    comm_write (comm, "\r\n", 1);
    return 0;
}

int main (int argc, char* argv[]) {
    process_command_line (argc, argv); // calls mudmux_init() when returning

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        SPDLOG_ERROR ("WSAStartup failed");
        return EXIT_FAILURE;
    }
    SPDLOG_INFO ("Winsock initialized: version {}.{}", LOBYTE(wsa_data.wVersion), HIBYTE(wsa_data.wVersion));
#endif

    // create server context

    // register hooks
    mudmux_register_hook (MUDMUX_HOOK_CONNECT, on_connect);
    mudmux_register_hook (MUDMUX_HOOK_MESSAGE_INBOUND, on_message_inbound);

    // [optional] connect any additional transports (e.g., listening sockets, etc.) here

    // run infinite event loop until shutdown is requested
    int exit_code = mudmux_run(nullptr);
    mudmux_deinit();

    // cleanup server context

#ifdef _WIN32
    WSACleanup();
#endif
    return exit_code;
}

/**
 *  @brief Process command line arguments and set up logging.
 *  Exits the program on error or if --help or --version is specified.
 */
static void process_command_line (int argc, char* argv[]) {
    int log_level = spdlog::level::warn; // keep quieter defaults for non-Debug builds

    argparse::ArgumentParser program (argv[0], "1.0");
    program.add_argument("-f", "--config").metavar("FILE").default_value(std::string("mud.conf"))
        .help("specify configuration file");
    program.add_argument("-V", "--verbose").default_value(false).implicit_value(true).nargs(0)
        .action([&](const auto & /*unused*/) {
            if (log_level > spdlog::level::trace)
                log_level--;
        })
        .append() // -V: info, -VV: debug, -VVV: trace (debug/trace disabled in release builds)
        .help("increase verbosity of logging output");

    try {
        program.parse_args (argc, argv);
        spdlog::set_level(static_cast<spdlog::level::level_enum>(log_level));
        mudmux_set_log_level(log_level); // set shared library log level to match main program
        SPDLOG_DEBUG ("log level set to {}", spdlog::level::to_string_view(spdlog::get_level()));
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl << program;
        std::exit(EXIT_FAILURE);
    }

    if (program.get<bool>("--version")) {
        std::cout << "1.0" << std::endl;
        std::exit(EXIT_SUCCESS);
    }

    if (program.is_used("--config")) {
        std::string config_file = program.get<std::string>("--config");
        std::ifstream input(config_file);
        if (!input) {
            SPDLOG_ERROR ("failed to open configuration file: {}", config_file);
            std::exit(EXIT_FAILURE);
        }

        std::stringstream buffer;
        buffer << input.rdbuf();
        std::string config_yaml = buffer.str();

        if (!mudmux_init(config_yaml.c_str())) {
            std::exit(EXIT_FAILURE);
        }
    }
    else {
        SPDLOG_INFO ("no configuration file specified, using defaults");
        if (!mudmux_init(nullptr)) {
            std::exit(EXIT_FAILURE);
        }
    }
}
