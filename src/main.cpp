// main.cpp
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async/async_runtime.h"
#include "comm/console.h"
#include "comm/listen.h"

#include <iostream>
#include <string>
#include <argparse/argparse.hpp>
#include <yaml-cpp/yaml.h>

static void process_command_line (int argc, char* argv[]);

int main (int argc, char* argv[]) {
    process_command_line (argc, argv);

    // initialize subsystems
    auto runtime = async_runtime_init(nullptr);
    bool success = runtime
        && comm_init_console(runtime);
        // && comm_init_listening_port (runtime, 4000, nullptr);
    if (!success) {
        SPDLOG_ERROR ("failed to initialize");
        async_runtime_deinit(runtime);
        return EXIT_FAILURE;
    }

    // main event loop
    SPDLOG_INFO ("entering event loop");
    io_event_t events[64];
    bool will_shutdown = false;
    while (!will_shutdown) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(
            runtime, events, sizeof(events) / sizeof(events[0]), nullptr);
        if (num_events < 0) {
            SPDLOG_CRITICAL ("async_runtime_wait failed"); // TODO: initiate retry or shutdown
            break;
        }
        SPDLOG_DEBUG ("async_runtime_wait returned {} events", num_events);

        // process console input (always check for console input, even if no events were returned)
        comm_process_console_input (runtime, &will_shutdown);

        // process I/O events (non-blocking)
        for (int i = 0; i < num_events; ++i) {
#ifndef NDEBUG
            auto& event = events[i];
            SPDLOG_DEBUG ("event: fd={}, event_type={}, bytes_transferred={}",
                event.fd, event.event_type, event.bytes_transferred);
#endif
        }
    }
    SPDLOG_INFO ("exited event loop");

    comm_shutdown_console (runtime);
    async_runtime_deinit (runtime);
    return EXIT_SUCCESS;
}

/**
 *  @brief Process command line arguments and set up logging.
 *  Exits the program on error or if --help or --version is specified.
 */
static void process_command_line (int argc, char* argv[]) {
    int log_level = spdlog::level::warn; // default log level

    argparse::ArgumentParser program (PACKAGE, VERSION);
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
        program.parse_args(argc, argv);
        spdlog::set_level(static_cast<spdlog::level::level_enum>(log_level));
        SPDLOG_DEBUG ("log level set to {}", spdlog::level::to_string_view(spdlog::get_level()));
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl << program;
        std::exit(EXIT_FAILURE);
    }

    if (program.get<bool>("--version")) {
        std::cout << PACKAGE << " version " << VERSION << std::endl;
        std::exit(EXIT_SUCCESS);
    }

    if (program.is_used("--config")) {
        std::string config_file = program.get<std::string>("--config");
        try {
            YAML::Node config = YAML::LoadFile(config_file);
            SPDLOG_INFO ("loaded configuration file: {}", config_file);
        }
        catch (const YAML::BadFile& e) {
            SPDLOG_ERROR ("failed to load configuration file: {}", e.what());
            std::exit(EXIT_FAILURE);
        }
        catch (const YAML::ParserException& e) {
            SPDLOG_ERROR ("failed to parse configuration file: {}", e.what());
            std::exit(EXIT_FAILURE);
        }
    }
}
