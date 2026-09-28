# Example programs (built with the tools).

add_executable(cfw-ui-gallery ui-gallery/main.cpp)
target_link_libraries(cfw-ui-gallery PRIVATE cfw-app cfw-io)
if(WIN32)
    set_target_properties(cfw-ui-gallery PROPERTIES WIN32_EXECUTABLE ON)
endif()
