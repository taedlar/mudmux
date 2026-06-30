# lib/comm/CMakeLists.txt

add_library(comm OBJECT
    console.cpp
    listen.cpp
)
add_library(mudmux::comm ALIAS comm)
target_compile_definitions(comm PRIVATE mudmux_EXPORTS)

target_link_libraries(comm PRIVATE
    spdlog::spdlog
    mudmux::async
    mudmux::headers
)
