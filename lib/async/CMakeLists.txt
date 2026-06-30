# lib/async/CMakeLists.txt

add_library(async OBJECT)
add_library(mudmux::async ALIAS async)

target_compile_definitions(async PRIVATE mudmux_EXPORTS)
target_sources(async INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_SOURCE_DIR}/lib
    FILES
        sync.h
        async_queue.h
        async_worker.h
        async_runtime.h
        console_mode.h
        console_worker.h
        socket_intf.h
)

target_sources(async PRIVATE
    socket_intf.cpp
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

target_link_libraries(async PRIVATE spdlog::spdlog)

# Platform-specific libraries
if (UNIX)
    target_link_libraries(async PRIVATE pthread)
endif()
