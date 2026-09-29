# cfw-net: event loop, executor, TCP, WebSocket client and server.
# Depends on cfw-core. Platform code lives in Socket{Win32,Posix}.cpp.

set(CFW_NET_PLATFORM_SOURCES)
set(CFW_NET_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_NET_PLATFORM_SOURCES src/SocketWin32.cpp src/TlsSchannel.cpp)
    list(APPEND CFW_NET_PLATFORM_LIBS ws2_32 secur32 crypt32 ncrypt)
else()
    # TLS through the system's OpenSSL, loaded at runtime (docs/decisions/0013):
    # nothing to link but dlopen.
    list(APPEND CFW_NET_PLATFORM_SOURCES src/SocketPosix.cpp src/TlsOpenSsl.cpp)
    list(APPEND CFW_NET_PLATFORM_LIBS ${CMAKE_DL_LIBS})
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

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-net WebSocketProtocolTest)
    cfw_add_test(cfw-net EventLoopTest)
    cfw_add_test(cfw-net WebSocketTest)
    cfw_add_test(cfw-net HttpTest)
    cfw_add_test(cfw-net TlsTest)
    # A few seconds by default; CFW_SOAK_SECONDS makes it as long as wanted.
    cfw_add_test(cfw-net SoakNetTest)
    set_tests_properties(SoakNetTest PROPERTIES LABELS "cfw-net;soak" TIMEOUT 600)
    target_link_libraries(TlsTest PRIVATE cfw-io)
endif()
