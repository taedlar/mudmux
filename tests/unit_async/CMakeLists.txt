# tests/unit_async/CMakeLists.txt

add_executable(unit_async
  test_async_primitives.cpp
)

target_link_libraries(unit_async PRIVATE
    spdlog::spdlog
    mudmux::headers
    ${MUDMUX_LIB_OBJECTS}
    GTest::gtest_main
)
target_compile_definitions(unit_async PRIVATE MUDMUX_STATIC_DEFINE)

gtest_discover_tests(unit_async DISCOVERY_TIMEOUT 30)
