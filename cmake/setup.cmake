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

# Optional Boost dependency
# set(Boost_DEBUG ON)
if (BOOST_ROOT)
    # If BOOST_ROOT is set, we assume the user has Boost installed and wants to use it.
    message(STATUS "Using user-specified BOOST_ROOT: ${BOOST_ROOT}")
    if (POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD) # Allow module mode for old FindBoost.cmake
    endif()
    set(Boost_NO_SYSTEM_PATHS ON) # Prevent FindBoost from searching system paths
    find_package(Boost REQUIRED)
else()
    # Modern CMake removes the FindBoost.cmake module and rely on Boost's config files.
    # In this case, we'll need to supply hints for popular Linux distributions.
    find_package(Boost CONFIG HINTS
        /usr/lib/x86_64-linux-gnu/cmake # Debian/Ubuntu
        /usr/lib64/cmake # Fedora/Red Hat
        /usr/local/lib/cmake # Custom installations
        /opt/local/lib/cmake # MacPorts
    )
endif()
if (Boost_FOUND)
    set(HAVE_BOOST TRUE) # for config header
endif()

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
