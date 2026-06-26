# CMakeLists.txt
cmake_minimum_required(VERSION 3.28)

# project settings
project(mudmux
    VERSION 0.1
    DESCRIPTION "A transport-layer host for MUDs"
    HOMEPAGE_URL "https://github.com/taedlar/mudmux"
    LANGUAGES C CXX
)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CTest) # optionally enable testing with -DBUILD_TESTING=ON
include(utils)

# compiler settings
if(MSVC)
    add_compile_options(/W4 /WX /Zc:__cplusplus /EHs /permissive- /utf-8 /nologo)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# check for standard functions
include(CheckSymbolExists)
check_symbol_exists(gettimeofday sys/time.h HAVE_GETTIMEOFDAY)

# find or fetch dependencies
find_boost(1.70 COMPONENTS filesystem)
find_boost_headers(boost/filesystem.hpp)
find_package(OpenSSL)
include(fetch-settings) # pulls in FetchContent and declares dependencies
FetchContent_MakeAvailable(argparse spdlog)

# generate config.h from config.h.in
configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

# add subdirectories
add_subdirectory(lib/async)
add_subdirectory(src)

# add tests
if (BUILD_TESTING)
    FetchContent_MakeAvailable(GTest) # always fetch GoogleTest (as recommended by Google)
    if (NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR "GTest not found and could not be fetched."
            " Please check your internet connection or disable testing with -DBUILD_TESTING=OFF.")
    endif()
    include(GoogleTest)
endif()
