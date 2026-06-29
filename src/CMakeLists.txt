# src/CMakeLists.txt

add_library(mudmux SHARED mudmux.cpp)
add_library(mudmux::mudmux ALIAS mudmux)

target_link_libraries(mudmux PRIVATE
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    mudmux::async
    mudmux::comm
    # OpenSSL::SSL
    # OpenSSL::Crypto
)

file(GLOB mudmux_headers include/*.h)
target_sources(mudmux PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        ${mudmux_headers}
)

# example server using mudmux library
add_executable(example_server main.cpp)
target_link_libraries(example_server PRIVATE
    spdlog::spdlog
    argparse::argparse
    mudmux::mudmux
)
