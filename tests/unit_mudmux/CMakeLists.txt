# tests/unit_mudmux/CMakeLists.txt

add_executable(unit_mudmux
    test_mudmux.cpp
)

target_link_libraries(unit_mudmux
    spdlog::spdlog
    mudmux::mudmux
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::applink
)

add_custom_command(TARGET unit_mudmux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:unit_mudmux> $<TARGET_RUNTIME_DLLS:unit_mudmux>
    COMMAND_EXPAND_LISTS
)

gtest_discover_tests(unit_mudmux)
