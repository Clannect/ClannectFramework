# cfw-app: the application layer (spec §4.9): UiWindow, a native window
# showing a cfw-ui surface, and GlUiWindow, the same with OpenGL views in it.

cfw_add_module(cfw-app
    SOURCES
        src/GlUiWindow.cpp
        src/UiWindow.cpp
    DEPENDS
        cfw-platform
        cfw-ui
)

if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-app UiWindowTest)
    cfw_add_test(cfw-app GlUiWindowTest)
endif()
