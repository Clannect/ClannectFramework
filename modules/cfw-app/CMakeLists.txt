# cfw-app: the application layer (spec §4.9): UiWindow, a native window
# showing a cfw-ui surface, and GlUiWindow, the same with OpenGL views in it.

# Assistive technology: MSAA on Windows; AT-SPI on Linux comes next.
if(WIN32)
    set(CFW_APP_ACCESSIBILITY src/AccessibilityWin32.cpp)
else()
    set(CFW_APP_ACCESSIBILITY src/AccessibilityNone.cpp)
endif()

cfw_add_module(cfw-app
    SOURCES
        src/Desktop.cpp
        src/GlUiWindow.cpp
        src/UiWindow.cpp
        ${CFW_APP_ACCESSIBILITY}
    DEPENDS
        cfw-io
        cfw-platform
        cfw-ui
)
if(WIN32)
    target_link_libraries(cfw-app PRIVATE comdlg32 shell32 oleacc oleaut32 ole32)
endif()

if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-app UiWindowTest)
    cfw_add_test(cfw-app GlUiWindowTest)
    cfw_add_test(cfw-app DesktopTest)
    if(WIN32)
        cfw_add_test(cfw-app AccessibilityWin32Test)
        target_link_libraries(AccessibilityWin32Test PRIVATE oleacc oleaut32 ole32)
    endif()
endif()
