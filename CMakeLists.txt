# CMakeLists.txt
cmake_minimum_required(VERSION 3.28)

# project settings
project(mudmux
    VERSION 0.1
    DESCRIPTION "A transport-layer host for MUDs"
    HOMEPAGE_URL "https://github.com/taedlar/mudmux"
    LANGUAGES CXX
)
set(MUDMUX_MAIN_PROJECT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(MUDMUX_MAIN_PROJECT ON)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

include(CTest) # optionally enable testing with -DBUILD_TESTING=ON
include(CMakeDependentOption)
include(utils)

cmake_dependent_option(MUDMUX_BUILD_TESTS
  "Enable mudmux tests" ON
  "BUILD_TESTING;MUDMUX_MAIN_PROJECT" OFF)

# compiler settings
if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /nologo)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
add_compile_definitions($<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>) # compile-time log level, default: >= SPDLOG_LEVEL_INFO

# check for standard headers
include(CheckIncludeFiles)
check_include_files(stdbool.h HAVE_STDBOOL_H)
check_include_files(unistd.h HAVE_UNISTD_H)

# check for standard functions
include(CheckSymbolExists)
check_symbol_exists(gettimeofday sys/time.h HAVE_GETTIMEOFDAY)

# find or fetch dependencies
find_boost(1.70 COMPONENTS json)
find_boost_headers(boost/json.hpp)
find_package(OpenSSL COMPONENTS SSL Crypto REQUIRED)
include(fetch-settings) # pulls in FetchContent and declares dependencies
FetchContent_MakeAvailable(argparse spdlog yaml-cpp)

# patches for dependencies
if (MSVC AND TARGET yaml-cpp)
    target_compile_options(yaml-cpp PRIVATE /wd4067)
endif()

# generate config.h from config.h.in
configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

# add subdirectories
add_subdirectory(lib/async)
add_subdirectory(lib/comm)
add_subdirectory(src)

# add tests
if (BUILD_TESTING AND MUDMUX_BUILD_TESTS)
    FetchContent_MakeAvailable(GTest) # always fetch GoogleTest (as recommended by Google)
    if (NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR "GTest not found and could not be fetched."
            " Please check your internet connection or disable testing with -DBUILD_TESTING=OFF.")
    endif()
    include(GoogleTest)
endif()
