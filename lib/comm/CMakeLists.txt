# lib/comm/CMakeLists.txt

add_library(comm STATIC
    console.cpp
    listen.cpp
)
add_library(mudmux::comm ALIAS comm)

target_link_libraries(comm PRIVATE
    spdlog::spdlog
    mudmux::async
)
