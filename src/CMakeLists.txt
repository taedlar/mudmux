# src/CMakeLists.txt
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)

add_library(mudmux_headers INTERFACE)
add_library(mudmux SHARED
    mudmux.cpp
    hooks.cpp
)
add_library(mudmux::mudmux ALIAS mudmux)
add_library(mudmux::headers ALIAS mudmux_headers)
generate_export_header(mudmux)
target_link_libraries(mudmux PUBLIC mudmux_headers)
target_link_libraries(mudmux PRIVATE
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    mudmux::async
    mudmux::comm
    # OpenSSL::SSL
    # OpenSSL::Crypto
)

# headers
file(GLOB _mudmux_headers include/mudmux/*.h)
target_sources(mudmux_headers PUBLIC
    FILE_SET HEADERS
        BASE_DIRS include
        FILES ${_mudmux_headers}
    FILE_SET generated_headers
        TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}
        FILES ${CMAKE_CURRENT_BINARY_DIR}/mudmux_export.h
)

# example server using mudmux library
add_executable(example_server main.cpp)
target_link_libraries(example_server PRIVATE
    spdlog::spdlog
    argparse::argparse
    mudmux::mudmux
)
