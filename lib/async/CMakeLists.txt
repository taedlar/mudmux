# lib/async/CMakeLists.txt

add_library(async OBJECT)
add_library(mudmux::async ALIAS async)

target_sources(async INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_SOURCE_DIR}/lib
    FILES
        async_event.h
        async_queue.h
        async_runtime.h
        console_worker.h
        socket_intf.h
)
target_compile_definitions(async PRIVATE mudmux_EXPORTS)

target_sources(async PRIVATE
    socket_intf.cpp
    async_event.cpp
    async_queue.cpp
    console_worker.cpp
)

if (WIN32)
    target_sources(async PRIVATE
        async_runtime_iocp.cpp
    )
elseif (LINUX)
    target_sources(async PRIVATE
        async_runtime_epoll.cpp
    )
elseif(APPLE)
    target_sources(async PRIVATE
        async_runtime_poll.cpp
    )
endif()

target_link_libraries(async PRIVATE
    spdlog::spdlog
    mudmux::headers
)

# Platform-specific libraries
if (UNIX)
    target_link_libraries(async PRIVATE pthread)
endif()
