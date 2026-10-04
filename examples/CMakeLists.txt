# Example programs (built with the tools).

add_executable(cfw-ui-gallery ui-gallery/main.cpp)
target_link_libraries(cfw-ui-gallery PRIVATE cfw-app cfw-io)
cfw_configure_target(cfw-ui-gallery) # warnings and sanitizers, like the modules
if(WIN32)
    set_target_properties(cfw-ui-gallery PROPERTIES WIN32_EXECUTABLE ON)
endif()
if(CFW_INSTALL)
    install(TARGETS cfw-ui-gallery RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Plays a sound file through the real output device (run it by hand).
add_executable(cfw-play-audio play-audio/main.cpp)
target_link_libraries(cfw-play-audio PRIVATE cfw-audio cfw-io)
cfw_configure_target(cfw-play-audio)

# Prints every input event a window and the gamepads produce (run it by hand).
add_executable(cfw-input-events input-events/main.cpp)
target_link_libraries(cfw-input-events PRIVATE cfw-platform)
cfw_configure_target(cfw-input-events)
