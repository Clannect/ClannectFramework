# cfw-ui: the toolkit. Element tree, layout, input routing and focus, theme
# tokens, and the controls (spec §4.8). Headless: platform windows
# (cfw-platform) feed it events and paint it; nothing here touches the OS.

cfw_add_module(cfw-ui
    SOURCES
        src/Choices.cpp
        src/Chrome.cpp
        src/Controls.cpp
        src/Dialog.cpp
        src/Dock.cpp
        src/Element.cpp
        src/Icon.cpp
        src/PropertyGrid.cpp
        src/ScrollArea.cpp
        src/Shortcut.cpp
        src/Surface.cpp
        src/Accessibility.cpp
        src/AccessibleControls.cpp
        src/TextField.cpp
        src/Views.cpp
        src/Theme.cpp
    DEPENDS
        cfw-gfx
        cfw-text
)

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-ui UiTest)
    cfw_add_test(cfw-ui ShellTest)
    cfw_add_test(cfw-ui ChromeTest)
    cfw_add_test(cfw-ui TreeEditTest)
    cfw_add_test(cfw-ui DockTest)
    cfw_add_test(cfw-ui PropertyGridTest)
    cfw_add_test(cfw-ui DialogTest)
    cfw_add_test(cfw-ui DropTest)
    cfw_add_test(cfw-ui CompositionTest)
    cfw_add_test(cfw-ui AccessibilityTest)
    cfw_add_test(cfw-ui SoakUiTest)
    set_tests_properties(SoakUiTest PROPERTIES LABELS "cfw-ui;soak" TIMEOUT 600)
endif()
