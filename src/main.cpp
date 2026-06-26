// main.cpp
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async/async_queue.h"
#include "async/async_runtime.h"
#include "async/console_worker.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <argparse/argparse.hpp>
#include <yaml-cpp/yaml.h>

static void process_command_line(int argc, char* argv[]);

namespace fs = std::filesystem;

int main (int argc, char* argv[]) {
    // process command line arguments
    process_command_line(argc, argv);

    // initialize async runtime
    auto runtime = async_runtime_init();

    // initialize console worker
    auto console_queue = async_queue_create (100, 4096, ASYNC_QUEUE_DROP_OLDEST);
    if (!console_queue) {
        debug_error ("failed to create console queue");
        return EXIT_FAILURE;
    }
    auto console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);

    // main event loop
    debug_info ("mudmux starting event loop");
    io_event_t events[64];
    bool will_shutdown = false;
    while (!will_shutdown) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(runtime, events, 64, nullptr);
        if (num_events < 0) {
            debug_error ("async_runtime_wait failed");
            break;
        }
        debug_info ("async_runtime_wait returned {} events", num_events);

        // process events
        for (int i = 0; i < num_events; ++i) {
            auto& event = events[i];
            debug_info ("event: fd={}, event_type={}, bytes_transferred={}", event.fd, event.event_type, event.bytes_transferred);
            if (console_worker_take_eof (console_ctx)) {
                if (console_ctx->console_type == CONSOLE_TYPE_REAL) {
                    // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                    debug_info ("console EOF detected, re-initializing console worker");
                    console_worker_destroy(console_ctx);
                    console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
                }
                else {
                    debug_info ("console EOF detected, shutting down");
                    will_shutdown = true;
                }
            }
        }
    }

    // shutdown console worker (gracefully)
    debug_info ("shutting down console worker");
    bool stopped = console_worker_shutdown(console_ctx, 5000);
    if (!stopped) {
        console_worker_destroy(console_ctx);
        async_queue_destroy(console_queue);
    }

    debug_info ("shutting down mudmux");
    async_runtime_deinit (runtime);
    return EXIT_SUCCESS;
}

static void process_command_line(int argc, char* argv[]) {
    argparse::ArgumentParser program (PACKAGE, VERSION);

    program.add_argument("-f", "--config").metavar("FILE").default_value(std::string("mud.conf"))
        .help("specify configuration file");

    try {
        program.parse_args(argc, argv);
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
            debug_info ("loaded configuration file: {}", config_file);
        }
        catch (const YAML::BadFile& e) {
            debug_error("failed to load configuration file {}: {}", config_file, e.what());
            std::exit(EXIT_FAILURE);
        }
        catch (const YAML::ParserException& e) {
            debug_error("failed to parse configuration file {}: {}", config_file, e.what());
            std::exit(EXIT_FAILURE);
        }
    }
}
