# src/CMakeLists.txt

include(GenerateExportHeader)

# public headers
add_library(mudmux_headers INTERFACE)
add_library(mudmux::headers ALIAS mudmux_headers)
file(GLOB _mudmux_public_headers include/mudmux/*.h)
target_sources(mudmux_headers PUBLIC
    FILE_SET HEADERS
        BASE_DIRS include
        FILES ${_mudmux_public_headers}
    FILE_SET generated_headers
        TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}
        FILES ${CMAKE_CURRENT_BINARY_DIR}/mudmux_export.h
)

# main objects
add_library(mudmux_core OBJECT
    mudmux.cpp
    hooks.cpp
)
add_library(mudmux::core ALIAS mudmux_core)
target_link_libraries(mudmux_core PUBLIC
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    mudmux::headers
    mudmux::async
    mudmux::comm
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_definitions(mudmux_core PRIVATE mudmux_EXPORTS)

# testees (static link-time dependencies for unit tests)
set (MUDMUX_LIB_OBJECTS
    mudmux::core
    mudmux::async
    mudmux::comm
    PARENT_SCOPE # export to parent score for unit-tests
)

# shared library
add_library(mudmux SHARED dllmain.cpp)
target_link_libraries(mudmux PRIVATE ${MUDMUX_LIB_OBJECTS})
target_link_libraries(mudmux PUBLIC mudmux::headers)
add_library(mudmux::mudmux ALIAS mudmux)
generate_export_header(mudmux)
