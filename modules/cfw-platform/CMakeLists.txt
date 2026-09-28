# cfw-platform: native windows, input, cursors, clipboard (spec §4.7).
# Win32 on Windows; X11 (the system's Xlib) on other Unix desktops when it is
# installed; otherwise a stub whose Window::create() reports Unsupported, so
# headless builds (the game server) still link.

set(CFW_PLATFORM_SOURCES src/Window.cpp)
set(CFW_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_PLATFORM_SOURCES src/WindowWin32.cpp)
    list(APPEND CFW_PLATFORM_LIBS user32 gdi32)
    set(CFW_PLATFORM_BACKEND "Win32")
else()
    find_package(X11 QUIET)
    if(X11_FOUND)
        list(APPEND CFW_PLATFORM_SOURCES src/WindowX11.cpp)
        list(APPEND CFW_PLATFORM_LIBS ${X11_LIBRARIES})
        set(CFW_PLATFORM_BACKEND "X11")
    else()
        list(APPEND CFW_PLATFORM_SOURCES src/WindowStub.cpp)
        set(CFW_PLATFORM_BACKEND "none")
    endif()
endif()
message(STATUS "cfw-platform: window system backend: ${CFW_PLATFORM_BACKEND}")

cfw_add_module(cfw-platform
    SOURCES
        ${CFW_PLATFORM_SOURCES}
    DEPENDS
        cfw-image
)
target_link_libraries(cfw-platform PRIVATE ${CFW_PLATFORM_LIBS})
if(X11_FOUND AND NOT WIN32)
    target_include_directories(cfw-platform PRIVATE ${X11_INCLUDE_DIR})
endif()

# Tests (only when CFW builds them: not as a subproject). Without a display
# the window test reports that it skipped and passes.
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-platform WindowTest)
endif()
