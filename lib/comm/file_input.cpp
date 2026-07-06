#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "file_input.h"

#include <mutex>
#include <openssl/bio.h>

#include "abstract.h"
#include "inbound.h"
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
        BIO* rbio = comm_abstract_get_rbio (slot);
        if (!rbio) {
            SPDLOG_INFO ("file input closed for slot {}", slot);
            break;
        }
        
        // Read from file BIO
        int bytes_read = BIO_read(rbio, buffer, sizeof(buffer) - 1);
        
        if (bytes_read < 0) {
            SPDLOG_ERROR ("BIO_read failed for slot {}", slot);
            break;
        } else if (bytes_read == 0) {
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

extern "C" bool comm_init_async_file_input (async_runtime_t *runtime, int slot) {
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
    BIO* rbio = comm_abstract_get_rbio(slot);
    if (!rbio) {
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

extern "C" void comm_shutdown_async_file_input (void) {
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

extern "C" int comm_process_file_input (async_runtime_t *runtime, int slot, const io_event_t* event) {
    (void)event; // unused
    
    std::lock_guard<std::mutex> lock(file_input_mutex);
    
    if (!file_input_queue) {
        return 0;
    }
    
    // Drain completed lines from the queue
    char file_line_buffer[4096];
    while (async_queue_dequeue (file_input_queue, file_line_buffer, sizeof(file_line_buffer), nullptr)) {
        comm_invoke_inbound_message(runtime, slot, file_line_buffer, strlen(file_line_buffer));
    }
    
    // Check if file EOF was detected
    if (file_input_eof) {
        SPDLOG_INFO ("async file input EOF detected for slot {}, shutting down server", slot);
        mudmux_shutdown();
        return 0;
    }
    
    return 1;
}

extern "C" bool comm_has_file_inputs (void) {
    std::lock_guard<std::mutex> lock(file_input_mutex);
    return file_input_queue != nullptr;
}
