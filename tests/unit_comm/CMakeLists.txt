# tests/unit_comm/CMakeLists.txt

add_executable(unit_comm
  test_comm_abstract.cpp
  test_comm_inbound.cpp
  test_comm_input_mode.cpp
  test_comm_ssl.cpp
)

target_include_directories(unit_comm PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_compile_definitions(unit_comm PRIVATE MUDMUX_STATIC_DEFINE) # define to access private functions not exported in the shared library
target_link_libraries(unit_comm PRIVATE
    spdlog::spdlog
    mudmux::headers
    ${MUDMUX_LIB_OBJECTS}
    OpenSSL::SSL
    OpenSSL::applink
    GTest::gtest_main
)

gtest_discover_tests(unit_comm)
