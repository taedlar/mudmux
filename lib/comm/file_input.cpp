#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "file_input.hpp"

#include <mutex>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "abstract.hpp"
#include "inbound.hpp"
#include "async/async_queue.h"
#include "mudmux/hooks.h"
#include "mudmux/mudmux.h"

static std::mutex file_input_mutex;
static async_queue_t* file_input_queue{nullptr};
static std::thread* file_input_thread{nullptr};  // thread for file reading
static bool file_input_eof{false};  // true when file EOF is detected

/**
 * File input reader thread - reads from file BIO and enqueues data
 */
static void file_input_reader_thread (async_runtime_t* runtime, int slot, async_queue_t* queue, uintptr_t completion_key) {
    char buffer[4096];
    SPDLOG_INFO ("file input reader thread started for slot {}", slot);
    
    while (true) {
        size_t bytes_read = 0;
        {
            comm_abstract_ptr comm(slot, mud_logic_mutex);
            if (!comm || !comm->rbio) {
                SPDLOG_ERROR ("file input reader thread exiting: invalid comm slot {}", slot);
                break;
            }
            if (BIO_read_ex(comm->rbio, buffer, sizeof(buffer) - 1, &bytes_read) <= 0) {
                if (!BIO_should_retry(comm->rbio)) {
                    SPDLOG_ERROR ("BIO_read_ex() failed for slot {}: {}", slot, ERR_get_error());
                    break;
                }
            }
        }
        
        if (!bytes_read) {
            // EOF
            SPDLOG_INFO ("file EOF detected for slot {}", slot);
            {
                std::lock_guard<std::mutex> lock(file_input_mutex);
                file_input_eof = true;
            }
            async_runtime_post_completion (runtime, completion_key, 0);
            break;
        }
        
        // Null-terminate
        buffer[bytes_read] = '\0';
        
        // Enqueue data
        if (!async_queue_enqueue (queue, buffer, bytes_read + 1)) {
            SPDLOG_WARN("file queue full, dropping data");
        }
        
        // Post completion to wake main thread
        async_runtime_post_completion (runtime, completion_key, bytes_read);
    }
    
    SPDLOG_INFO("file input reader thread stopped");
}

bool comm_init_async_file_input (async_runtime_t *runtime, int slot) {
    std::lock_guard<std::mutex> lock(file_input_mutex);
    
    // create file input queue if it doesn't exist
    if (!file_input_queue) {
        file_input_queue = async_queue_create (100, 4096, ASYNC_QUEUE_DROP_OLDEST);
        if (!file_input_queue) {
            SPDLOG_ERROR ("failed to create file input queue");
            return false;
        }
    }
    
    // Check if slot has an rbio (file input)
    if (!comm_abstract_has_rbio(slot)) {
        SPDLOG_ERROR ("no file input detected for slot {} for async initialization", slot);
        return false;
    }
    
    // Start file input reader thread
    SPDLOG_DEBUG ("initializing async file input for slot {}, starting reader thread", slot);
    file_input_eof = false;
    
    file_input_thread = new std::thread(file_input_reader_thread, runtime, slot, file_input_queue, FILE_INPUT_COMPLETION_KEY(slot));
    if (!file_input_thread) {
        SPDLOG_ERROR ("failed to create file input thread");
        return false;
    }
    
    // invoke connect hook for file input user
    comm_invoke_connect (runtime, slot);
    
    return true;
}

void comm_shutdown_async_file_input (void) {
    std::lock_guard<std::mutex> lock(file_input_mutex);
    
    if (file_input_thread) {
        if (file_input_thread->joinable()) {
            file_input_thread->join();
        }
        delete file_input_thread;
        file_input_thread = nullptr;
    }
    
    if (file_input_queue) {
        async_queue_destroy(file_input_queue);
        file_input_queue = nullptr;
    }
    
    file_input_eof = false;
}

int comm_process_file_input (async_runtime_t *runtime, int slot, const io_event_t* event) {
    (void)event; // unused

    char file_line_buffer[4096];
    size_t file_line_len = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(file_input_mutex);
            if (!file_input_queue) {
                return 0;
            }
            if (!async_queue_dequeue (file_input_queue, file_line_buffer, sizeof(file_line_buffer), &file_line_len)) {
                break;
            }
        }

        if (file_line_len > 0 && file_line_buffer[file_line_len - 1] == '\0') {
            --file_line_len;
        }
        if (!comm_refill_inbound_buffers (slot, file_line_buffer, file_line_len)) {
            SPDLOG_WARN ("failed to refill inbound buffers for file input slot {}", slot);
            break;
        }
    }

    (void) comm_process_input (runtime, slot);

    {
        std::lock_guard<std::mutex> lock(file_input_mutex);
        if (file_input_eof) {
            SPDLOG_INFO ("async file input EOF detected for slot {}, shutting down server", slot);
            mudmux_shutdown();
            return 0;
        }
    }
    
    return 1;
}

bool comm_has_file_inputs (void) {
    std::lock_guard<std::mutex> lock(file_input_mutex);
    return file_input_queue != nullptr;
}
