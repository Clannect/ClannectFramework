# cfw-ui: the toolkit. Element tree, layout, input routing and focus, theme
# tokens, and the controls (spec §4.8). Headless: platform windows
# (cfw-platform) feed it events and paint it; nothing here touches the OS.

cfw_add_module(cfw-ui
    SOURCES
        src/Controls.cpp
        src/Element.cpp
        src/Surface.cpp
        src/Theme.cpp
    DEPENDS
        cfw-gfx
        cfw-text
)

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-ui UiTest)
endif()
