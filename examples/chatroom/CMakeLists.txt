# examples/chatroom/CMakeLists.txt

add_executable(chatroom main.cpp)

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
