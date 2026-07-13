# examples/chatroom/CMakeLists.txt

add_executable(chatroom main.cpp
    user.cpp)

target_link_libraries(chatroom PRIVATE
    spdlog::spdlog
    argparse::argparse
    mudmux::mudmux
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(chatroom PRIVATE OpenSSL::applink)
endif()

add_custom_command(TARGET chatroom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:chatroom> $<TARGET_RUNTIME_DLLS:chatroom>
    COMMAND_EXPAND_LISTS
)
