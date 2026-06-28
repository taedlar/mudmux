// main.cpp
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async/async_runtime.h"
#include "comm/console.h"

#include <iostream>
#include <string>
#include <argparse/argparse.hpp>
#include <yaml-cpp/yaml.h>

static void process_command_line(int argc, char* argv[]);

int main (int argc, char* argv[]) {
    // process command line arguments
    process_command_line(argc, argv);

    // initialize async runtime and subsystems
    auto runtime = async_runtime_init();
    bool success = runtime && comm_init_console(runtime);
    if (!success) {
        SPDLOG_ERROR ("failed to initialize console");
        async_runtime_deinit(runtime);
        return EXIT_FAILURE;
    }

    // main event loop
    SPDLOG_INFO ("mudmux starting event loop");
    io_event_t events[64];
    bool will_shutdown = false;
    while (!will_shutdown) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(runtime, events, 64, nullptr);
        if (num_events < 0) {
            SPDLOG_ERROR ("async_runtime_wait failed");
            break;
        }
        SPDLOG_DEBUG ("async_runtime_wait returned {} events", num_events);

        // process console input (always check for console input, even if no events were returned)
        comm_process_console_input (runtime, &will_shutdown);

        // process events
        for (int i = 0; i < num_events; ++i) {
            auto& event = events[i];
            SPDLOG_DEBUG ("event: fd={}, event_type={}, bytes_transferred={}", event.fd, event.event_type, event.bytes_transferred);
        }
    }

    // shutdown console (gracefully)
    SPDLOG_INFO ("shutting down console");
    comm_shutdown_console (runtime);

    SPDLOG_INFO ("shutting down mudmux");
    async_runtime_deinit (runtime);
    return EXIT_SUCCESS;
}

static void process_command_line(int argc, char* argv[]) {
    int log_level = spdlog::level::warn; // default log level

    argparse::ArgumentParser program (PACKAGE, VERSION);
    program.add_argument("-f", "--config").metavar("FILE").default_value(std::string("mud.conf"))
        .help("specify configuration file");
    program.add_argument("-V", "--verbose").default_value(false).implicit_value(true).nargs(0)
        .action([&](const auto & /*unused*/) { if (log_level > spdlog::level::trace) log_level--; })
        .append() // -V: info, -VV: debug, -VVV: trace
        .help("increase verbosity of logging output");

    try {
        program.parse_args(argc, argv);
        spdlog::set_level(static_cast<spdlog::level::level_enum>(log_level));
        SPDLOG_DEBUG ("log level set to {}", spdlog::level::to_string_view(spdlog::get_level()));
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
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
            SPDLOG_ERROR ("failed to load configuration file {}: {}", config_file, e.what());
            std::exit(EXIT_FAILURE);
        }
        catch (const YAML::ParserException& e) {
            SPDLOG_ERROR ("failed to parse configuration file {}: {}", config_file, e.what());
            std::exit(EXIT_FAILURE);
        }
    }
}
