# tests/unit_comm/CMakeLists.txt

add_executable(unit_comm
  test_comm_abstract.cpp
)

target_include_directories(unit_comm PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(unit_comm PRIVATE
    spdlog::spdlog
    mudmux::mudmux
    OpenSSL::SSL
    OpenSSL::applink
    GTest::gtest_main
)

add_custom_command(TARGET unit_comm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:unit_comm> $<TARGET_RUNTIME_DLLS:unit_comm>
    COMMAND_EXPAND_LISTS
)

gtest_discover_tests(unit_comm)
