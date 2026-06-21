# lib/async/CMakeLists.txt

add_library(async STATIC)

target_sources(async PRIVATE
    socket_comm.cpp
    sync.cpp
    async_queue.cpp
    console_mode.cpp
    console_worker.cpp
)

if (WIN32)
    target_sources(async PRIVATE
        async_worker_win32.cpp
        async_runtime_iocp.cpp
    )
elseif (LINUX)
    target_sources(async PRIVATE
        async_worker_pthread.cpp
        async_runtime_epoll.cpp
    )
elseif(APPLE)
    target_sources(async PRIVATE
        async_worker_pthread.cpp
        async_runtime_poll.cpp
    )
endif()

# Platform-specific libraries
if (UNIX)
    target_link_libraries(async PRIVATE pthread)
endif()

target_include_directories(async PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
