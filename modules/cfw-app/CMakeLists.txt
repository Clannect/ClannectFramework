# cfw-app: the application layer (spec §4.9): UiWindow, a native window
# showing a cfw-ui surface, and GlUiWindow, the same with OpenGL views in it.

cfw_add_module(cfw-app
    SOURCES
        src/Desktop.cpp
        src/GlUiWindow.cpp
        src/UiWindow.cpp
    DEPENDS
        cfw-io
        cfw-platform
        cfw-ui
)
if(WIN32)
    target_link_libraries(cfw-app PRIVATE comdlg32 shell32)
endif()

if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-app UiWindowTest)
    cfw_add_test(cfw-app GlUiWindowTest)
    cfw_add_test(cfw-app DesktopTest)
endif()
