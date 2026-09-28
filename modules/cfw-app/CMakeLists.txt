# cfw-app: the application layer (spec §4.9). So far: UiWindow, a native
# window showing a cfw-ui surface.

cfw_add_module(cfw-app
    SOURCES
        src/UiWindow.cpp
    DEPENDS
        cfw-platform
        cfw-ui
)

if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-app UiWindowTest)
endif()
