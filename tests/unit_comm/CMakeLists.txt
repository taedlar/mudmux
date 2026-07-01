# tests/unit_comm/CMakeLists.txt

add_executable(unit_comm
  test_comm_abstract.cpp
)

target_include_directories(unit_comm PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(unit_comm PRIVATE
    spdlog::spdlog
    mudmux::mudmux
    OpenSSL::SSL
    GTest::gtest_main
)

gtest_discover_tests(unit_comm)
