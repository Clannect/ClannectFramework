# TLS interoperability between the backends: see TlsEchoServer.cpp. Built with
# the tests, run by CI (the linux workflow's windows-cross job).
if(WIN32)
    add_executable(SchannelClient SchannelClient.cpp)
    target_link_libraries(SchannelClient PRIVATE cfw-net)
    cfw_configure_target(SchannelClient)
else()
    add_executable(TlsEchoServer TlsEchoServer.cpp)
    target_link_libraries(TlsEchoServer PRIVATE cfw-net)
    cfw_configure_target(TlsEchoServer)
endif()
