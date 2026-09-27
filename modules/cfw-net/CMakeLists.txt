# cfw-net: event loop, executor, TCP, WebSocket client and server.
# Depends on cfw-core. Platform code lives in Socket{Win32,Posix}.cpp.

set(CFW_NET_PLATFORM_SOURCES)
set(CFW_NET_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_NET_PLATFORM_SOURCES src/SocketWin32.cpp)
    list(APPEND CFW_NET_PLATFORM_LIBS ws2_32)
else()
    list(APPEND CFW_NET_PLATFORM_SOURCES src/SocketPosix.cpp)
endif()

find_package(Threads REQUIRED)

cfw_add_module(cfw-net
    SOURCES
        src/EventLoop.cpp
        src/Executor.cpp
        src/HttpBodyDecoder.cpp
        src/HttpClient.cpp
        src/Tcp.cpp
        src/WebSocket.cpp
        src/WebSocketFrame.cpp
        src/WebSocketHandshake.cpp
        ${CFW_NET_PLATFORM_SOURCES}
    DEPENDS
        cfw-core
)
target_link_libraries(cfw-net PRIVATE ${CFW_NET_PLATFORM_LIBS} Threads::Threads)

cfw_add_test(cfw-net WebSocketProtocolTest)
cfw_add_test(cfw-net EventLoopTest)
cfw_add_test(cfw-net WebSocketTest)
cfw_add_test(cfw-net HttpTest)
