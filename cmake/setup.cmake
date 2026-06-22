# cmake/setup.cmake

include(FetchContent)

FetchContent_Declare(
    GTest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    FIND_PACKAGE_ARGS
        NAMES GTest
)

FetchContent_Declare(
    OpenSSL
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG openssl-3.6.3
    FIND_PACKAGE_ARGS
        NAMES OpenSSL openssl
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.9.2
    FIND_PACKAGE_ARGS
        NAMES spdlog
)

# =========================
# CMake Dependency Provider
# =========================

macro(setup_provide_dependency method package)
    message(STATUS "Providing dependency '${package}'")
endmacro()

cmake_language(
    SET_DEPENDENCY_PROVIDER setup_provide_dependency
    SUPPORTED_METHODS
        FETCHCONTENT_MAKEAVAILABLE_SERIAL
)
