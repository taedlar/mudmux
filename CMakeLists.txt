# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(mudmux
    VERSION 0.1
    DESCRIPTION "A transport-layer multiplexer for MUDs"
    HOMEPAGE_URL "https://github.com/taedlar/mudmux"
    LANGUAGES C CXX
)

enable_testing()

# Setup dependencies
include(FetchContent)
FetchContent_MakeAvailable(GTest OpenSSL spdlog)
if (BOOST_ROOT)
    # If BOOST_ROOT is set, we assume the user has Boost installed and wants to use it.
    message(STATUS "Detected Boost from BOOST_ROOT: ${BOOST_ROOT}")
    cmake_policy (SET CMP0167 OLD) # Allow FindBoost to use BOOST_ROOT
    find_package(Boost REQUIRED)
else()
    find_package(Boost QUIET) # FindBoost.cmake or library lookup fallback
endif()
set(HAVE_BOOST $<BOOL:${Boost_FOUND}> CACHE BOOL "Boost libraries are available" FORCE)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

add_subdirectory(lib/async)
add_subdirectory(src)

if (BUILD_TESTING)
    # GTest is already made available via FetchContent
endif()
