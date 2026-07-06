# tests/unit_async/CMakeLists.txt

add_executable(unit_async
  test_async_primitives.cpp
)

target_link_libraries(unit_async PRIVATE
    spdlog::spdlog
    mudmux::async
    GTest::gtest_main
)

gtest_discover_tests(unit_async)
