# cmake/utils.cmake

# This function checks for the presence of Boost libraries and sets corresponding variables
# in the parent scope.
function(find_boost required_version)
    cmake_parse_arguments(PARSE_ARGN arg "" "" "COMPONENTS")
    if (NOT "headers" IN_LIST arg_COMPONENTS)
        list(APPEND arg_COMPONENTS "headers")
    endif()
    # Modern CMake removes the FindBoost.cmake module and rely on Boost's config files.
    # In this case, we'll need to supply hints for popular Linux distributions.
    find_package(Boost ${required_version} CONFIG GLOBAL
        HINTS
            /usr/lib/x86_64-linux-gnu/cmake # Debian/Ubuntu
            /usr/lib64/cmake # Fedora/Red Hat
            /usr/local/lib/cmake # Custom installations
            /opt/local/lib/cmake # MacPorts
        COMPONENTS ${arg_COMPONENTS}
    )
    if (Boost_FOUND)
        message(STATUS "Found Boost: ${Boost_DIR} (found version ${Boost_VERSION})")
        set(HAVE_BOOST TRUE PARENT_SCOPE)
    endif()
    foreach(component IN LISTS arg_COMPONENTS)
        if (TARGET Boost::${component})
            string(TOUPPER ${component} component)
            set(HAVE_BOOST_${component} TRUE PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

# This function checks for the presence of specific Boost headers and sets corresponding variables
# in the parent scope. It also handles the inclusion of Boost's interface include directories if
# the Boost::headers target is available.
function(find_boost_headers)
    include(CheckIncludeFileCXX)
    set(saved_required_includes ${CMAKE_REQUIRED_INCLUDES})
    set(boost_required_includes)

    if (TARGET Boost::headers)
        get_target_property(boost_headers_include_dirs Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
        if (boost_headers_include_dirs)
            list(APPEND boost_required_includes ${boost_headers_include_dirs})
        endif()
    endif()

    if (Boost_INCLUDE_DIRS)
        list(APPEND boost_required_includes ${Boost_INCLUDE_DIRS})
    endif()

    if (Boost_INCLUDE_DIR)
        list(APPEND boost_required_includes ${Boost_INCLUDE_DIR})
    endif()

    if (boost_required_includes)
        list(REMOVE_DUPLICATES boost_required_includes)
        list(APPEND CMAKE_REQUIRED_INCLUDES ${boost_required_includes})
    endif()

    foreach(header IN LISTS ARGN)
        string(REPLACE "/" "_" header_var ${header})
        string(REPLACE "." "_" header_var ${header_var})
        string(TOUPPER ${header_var} header_var)
        check_include_file_cxx(${header} HAVE_${header_var})
        if (HAVE_${header_var})
            set(HAVE_${header_var} TRUE PARENT_SCOPE)
        endif()
    endforeach()

    set(CMAKE_REQUIRED_INCLUDES ${saved_required_includes})
endfunction()
