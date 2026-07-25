// main.cpp

#include <csignal>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "mudmux/comm.h"
#include "mudmux/hooks.h"
#include "mudmux/mudmux.h"
#include "command.hpp"
#include "user.hpp"

static void sigint_handler (int signal);
static void process_command_line (int argc, char* argv[]);

static int on_connect (void*, int slot, void* data, size_t len) {
    std::string entry_name{static_cast<const char*>(data), len};
    SPDLOG_INFO ("New connection on slot {} from entry '{}'", slot, entry_name);
    while (slot >= static_cast<int>(User::slots.size()))
        User::slots.resize(slot + 32);
    User::slots[slot] = std::make_shared<User>(slot);
    if (entry_name != "-") {
        // comm_enable_telnet (slot); // enable TELNET for non-console connections
        // comm_enable_tls (slot); // enable TLS for secure connections
        comm_enable_websocket (slot, "telnet.ietf.org, telnet.mudstandards.org"); // enable WebSocket for web clients
    }
    comm_enable_virtual_terminal (slot); // enable ANSI/VT100 processing for console and TELNET connections
    comm_enable_prompt (slot, true); // enable prompt for console user
    comm_set_line_input (slot, true); // enable line input mode for console user
    User::slots[slot]->logon(); // prompt for username
    return 0;
}

static int on_message_inbound (void*, int slot, void* data, size_t size) {
    std::string message(static_cast<char*>(data), size);
    if (slot < static_cast<int>(User::slots.size()) && User::slots[slot]) {
        User::slots[slot]->dispatchInboundMessage(message);
    }
    return 0;
}

static int on_prompt (void*, int slot, void*, size_t) {
    if (slot < static_cast<int>(User::slots.size()) && User::slots[slot]) {
        User::slots[slot]->prompt(); // display prompt for user input
    }
    return 0;
}

static int on_disconnect (void*, int slot, void*, size_t) {
    if (slot < static_cast<int>(User::slots.size()) && User::slots[slot]) {
        User::slots[slot]->disconnect(); // mark user as disconnected
        User::slots[slot].reset(); // remove user from the list
    }
    return 0;
}

int main (int argc, char* argv[]) {
    std::signal(SIGINT, sigint_handler);
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
    mudmux_register_hook (HOOK_CONNECT, on_connect);
    mudmux_register_hook (HOOK_MESSAGE_INBOUND, on_message_inbound);
    mudmux_register_hook (HOOK_DISCONNECT, on_disconnect);
    mudmux_register_hook (HOOK_PROMPT, on_prompt);

    // initialize chatroom command handlers
    Command::initialize();

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
    program.add_argument("-i", "--input").metavar("FILE").default_value(std::string("stdin"))
        .help("specify input file (default: stdin)");
    program.add_argument("-o", "--output").metavar("FILE").default_value(std::string("stdout"))
        .help("specify output file (default: stdout)");
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

    // call mudmux_init() to process configuration file (or use defaults if no file is specified)
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

    if (program.is_used("--input") || program.is_used("--output")) {
        // standard input/output overrides console mode (no re-connection after EOF)
        mudmux_enable_console(false);
        std::string input_file_str = program.get<std::string>("--input");
        std::string output_file_str = program.get<std::string>("--output");
        const char* input_file = (input_file_str != "stdin") ? input_file_str.c_str() : nullptr; // stdin by default
        const char* output_file = (output_file_str != "stdout") ? output_file_str.c_str() : nullptr; // stdout by default
        if (comm_abstract_add_file(input_file, output_file, COMM_SLOT_CONSOLE, 0) < 0) { // set up file/stdin and stdout
            SPDLOG_ERROR ("failed to open input/output files");
            std::exit(EXIT_FAILURE);
        }
        // Enable standard input handling only for stdin (not for file input)
        if (!input_file)
            mudmux_enable_standard_input(true);
    }
}

static void sigint_handler (int signal) {
    SPDLOG_INFO ("SIGINT received, shutting down...");
    (void)signal; // unused
    mudmux_shutdown();
}
