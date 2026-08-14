# lib/comm/CMakeLists.txt

add_library(comm OBJECT
    abstract.cpp
    accept.cpp
    console.cpp
    current_slot.cpp
    file_input.cpp
    input_mode.cpp
    inbound.cpp
    outbound.cpp
    telnet.cpp
    websocket.cpp
    ssl.cpp
    ssl.hpp
)
add_library(mudmux::comm ALIAS comm)
target_include_directories(comm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_sources(comm INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
    FILES
        abstract.hpp
        accept.hpp
        console.hpp
        current_slot.hpp
        file_input.hpp
        input_mode.hpp
        inbound.hpp
        outbound.hpp
        websocket.hpp
)
target_compile_definitions(comm PRIVATE mudmux_EXPORTS)

target_link_libraries(comm PRIVATE
    spdlog::spdlog
)

target_link_libraries(comm PUBLIC
    mudmux::async
    mudmux::headers
    OpenSSL::SSL
)
