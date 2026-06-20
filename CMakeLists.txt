# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(mudmux CXX)

enable_testing()

include(FetchContent)
FetchContent_MakeAvailable(GTest OpenSSL spdlog)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

add_subdirectory(src)

if (BUILD_TESTING)
    # GTest is already made available via FetchContent
endif()
