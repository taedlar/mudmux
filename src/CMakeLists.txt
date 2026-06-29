# src/CMakeLists.txt

add_executable(mudmux main.cpp)

target_link_libraries(mudmux PRIVATE
    spdlog::spdlog
    argparse::argparse
    yaml-cpp::yaml-cpp
    mudmux::async
    mudmux::comm
    OpenSSL::SSL
    OpenSSL::Crypto
)
