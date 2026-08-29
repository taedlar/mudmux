# CMakeLists.txt
cmake_minimum_required(VERSION 3.28)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# project settings
# ================
project(mudmux
    VERSION 0.1
    DESCRIPTION "A transport-layer library for MUDs"
    HOMEPAGE_URL "https://github.com/taedlar/mudmux"
    LANGUAGES CXX
)

include(CTest) # optionally enable testing with -DBUILD_TESTING=ON
include(CMakeDependentOption)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(utils)

# options
cmake_dependent_option(MUDMUX_BUILD_TESTS
  "Enable mudmux tests" ON
  "BUILD_TESTING;mudmux_IS_TOP_LEVEL" OFF)
cmake_dependent_option(MUDMUX_BUILD_EXAMPLES
  "Enable mudmux examples" ON
  "mudmux_IS_TOP_LEVEL" OFF)

# compiler settings
if(MSVC)
    add_compile_options(/W3 /permissive- /EHsc /utf-8 /nologo)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
add_compile_definitions($<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>) # compile-time log level, default: >= SPDLOG_LEVEL_INFO

# dependencies
# ============
include(CheckIncludeFiles)
include(CheckSymbolExists)
check_include_files(stdbool.h HAVE_STDBOOL_H)
check_include_files(termios.h HAVE_TERMIOS_H)
check_include_files(unistd.h HAVE_UNISTD_H)
check_include_files(arpa/telnet.h HAVE_ARPA_TELNET_H)
check_symbol_exists(gettimeofday sys/time.h HAVE_GETTIMEOFDAY)

find_boost(1.70 COMPONENTS json)
find_boost_headers(boost/json.hpp)
find_package(OpenSSL COMPONENTS SSL Crypto REQUIRED)
include(fetch-settings) # pulls in FetchContent and declares dependencies
FetchContent_MakeAvailable(spdlog yaml-cpp)
if(MUDMUX_BUILD_EXAMPLES)
    FetchContent_MakeAvailable(argparse)
endif()

# patches for dependencies
if (MSVC AND TARGET yaml-cpp)
    target_compile_options(yaml-cpp PRIVATE /wd4067)
endif()

# configure source files
# ======================
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_compile_definitions(HAVE_CONFIG_H)

# add subdirectories
# ==================
add_subdirectory(lib/async)
add_subdirectory(lib/comm)
add_subdirectory(src)
add_subdirectory(examples/chatroom)

# install and package configuration
# =================================
set(MUDMUX_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/mudmux")

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mudmuxConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mudmuxConfig.cmake
    INSTALL_DESTINATION ${MUDMUX_CMAKE_INSTALL_DIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mudmuxConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(TARGETS mudmux mudmux_headers
    EXPORT mudmuxTargets
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET generated_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mudmux
)
install(EXPORT mudmuxTargets
    FILE mudmuxTargets.cmake
    NAMESPACE mudmux::
    DESTINATION ${MUDMUX_CMAKE_INSTALL_DIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mudmuxConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mudmuxConfigVersion.cmake
    DESTINATION ${MUDMUX_CMAKE_INSTALL_DIR}
)
# add tests
if (MUDMUX_BUILD_TESTS)
    FetchContent_MakeAvailable(GTest) # always fetch GoogleTest (as recommended by Google)
    if (NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR "GTest not found and could not be fetched."
            " Please check your internet connection or disable testing with -DBUILD_TESTING=OFF.")
    endif()
    include(GoogleTest)
    add_subdirectory(tests/unit_async)
    add_subdirectory(tests/unit_comm)
    add_subdirectory(tests/unit_mudmux)
endif()
