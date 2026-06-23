# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)

# Project settings
project(mudmux
    VERSION 0.1
    DESCRIPTION "A transport-layer host for MUDs"
    HOMEPAGE_URL "https://github.com/taedlar/mudmux"
    LANGUAGES C CXX
)
enable_testing()

# Compiler settings
if(MSVC)
    add_compile_options(/W4 /WX /Zc:__cplusplus /EHs /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	add_compile_options(-Wall -Werror -Wpedantic)
endif()

# Configure dependencies
include(FetchContent)
FetchContent_MakeAvailable(GTest OpenSSL spdlog)

# Generate config.h from config.h.in
configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

# Add subdirectories
add_subdirectory(lib/async)
add_subdirectory(src)

if (BUILD_TESTING)
    # GTest is already made available via FetchContent
endif()
