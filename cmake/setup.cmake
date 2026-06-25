# cmake/setup.cmake

# =========================
# cmake policy settings
# =========================

if (POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW) # Allow uppercase <package>_ROOT for find_package
endif()

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD) # Allow module mode for old FindBoost.cmake
endif()

# =========================
# CMake Dependency Provider
# =========================

function(setup_provide_dependency method package)
    message(STATUS "Providing dependency '${package}'")
endfunction()

cmake_language(
    SET_DEPENDENCY_PROVIDER setup_provide_dependency
    SUPPORTED_METHODS
        FETCHCONTENT_MAKEAVAILABLE_SERIAL
)
