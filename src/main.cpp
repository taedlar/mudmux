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

static int on_connect (void*, int, void* data, size_t) {
    SPDLOG_INFO ("New connection established");
    comm_abstract_t* comm = static_cast<comm_abstract_t*>(data);
    comm_write (comm, "Welcome to mudmux!\n", 0);
    return 0;
}

static int on_message_inbound (void*, int, void* data, size_t size) {
    std::string message(static_cast<char*>(data), size);
    std::cout << "Received message: " << message << std::endl;
    return 0;
}

int main (int argc, char* argv[]) {
    process_command_line (argc, argv); // calls mudmux_init() when returning

    // create server context and register transport layer hooks
    mudmux_register_hook (MUDMUX_HOOK_CONNECT, on_connect);
    mudmux_register_hook (MUDMUX_HOOK_MESSAGE_INBOUND, on_message_inbound);

    // run infinite event loop until shutdown is requested
    int exit_code = mudmux_run(nullptr);
    mudmux_deinit();

    // cleanup server context

    return exit_code;
}

/**
 *  @brief Process command line arguments and set up logging.
 *  Exits the program on error or if --help or --version is specified.
 */
static void process_command_line (int argc, char* argv[]) {
    int log_level = spdlog::level::warn; // default log level

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
