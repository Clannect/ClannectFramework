# cfw-platform: native windows, input, cursors, clipboard.
# Win32 on Windows; X11 (the system's Xlib) on other Unix desktops when it is
# installed; otherwise a stub whose Window::create() reports Unsupported, so
# headless builds (the game server) still link.

set(CFW_PLATFORM_SOURCES src/Window.cpp src/Gamepad.cpp src/GlFunctions.cpp src/GlObjects.cpp)
set(CFW_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_PLATFORM_SOURCES src/WindowWin32.cpp src/GlContextWgl.cpp src/GamepadWin32.cpp)
    list(APPEND CFW_PLATFORM_LIBS user32 gdi32 opengl32 ole32 shell32 imm32 hid)
    set(CFW_PLATFORM_BACKEND "Win32")
elseif(APPLE)
    list(APPEND CFW_PLATFORM_SOURCES src/WindowCocoa.mm src/GlContextCocoa.mm src/GamepadCocoa.mm)
    cfw_objcxx_sources(src/WindowCocoa.mm src/GlContextCocoa.mm src/GamepadCocoa.mm)
    list(APPEND CFW_PLATFORM_LIBS "-framework AppKit" "-framework Carbon" "-framework GameController" ${CMAKE_DL_LIBS})
    set(CFW_PLATFORM_BACKEND "Cocoa")
else()
    # Gamepads are the kernel's (evdev) on Linux, with or without a window system.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND CFW_PLATFORM_SOURCES src/GamepadLinux.cpp)
    else()
        list(APPEND CFW_PLATFORM_SOURCES src/GamepadNone.cpp)
    endif()
    find_package(X11 QUIET GLOBAL)
    if(X11_FOUND)
        list(APPEND CFW_PLATFORM_SOURCES src/WindowX11.cpp src/GlContextGlx.cpp)
        list(APPEND CFW_PLATFORM_LIBS X11::X11 ${CMAKE_DL_LIBS})
        set(CFW_PLATFORM_BACKEND "X11")
    else()
        list(APPEND CFW_PLATFORM_SOURCES src/WindowStub.cpp src/GlContextStub.cpp)
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
# The backend, for the installed package's dependencies.
set(CFW_PLATFORM_BACKEND ${CFW_PLATFORM_BACKEND} PARENT_SCOPE)

# Tests (only when CFW builds them: not as a subproject). Without a display
# the window test reports that it skipped and passes.
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-platform WindowTest)
    if(WIN32)
        target_sources(WindowTest PRIVATE tests/Win32TestHelpers.cpp)
    endif()
    cfw_add_test(cfw-platform GlTest)
    cfw_add_test(cfw-platform PreeditTest)
    cfw_add_test(cfw-platform InputTest)
    cfw_add_test(cfw-platform GamepadTest)
    if(X11_FOUND AND NOT WIN32 AND NOT APPLE)
        cfw_add_test(cfw-platform X11ClipboardTest)
        target_include_directories(X11ClipboardTest PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(X11ClipboardTest PRIVATE ${X11_LIBRARIES})
        cfw_add_test(cfw-platform X11ImeTest)
        target_include_directories(X11ImeTest PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(X11ImeTest PRIVATE ${X11_LIBRARIES})
        cfw_add_test(cfw-platform X11DropTest)
        target_include_directories(X11DropTest PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(X11DropTest PRIVATE ${X11_LIBRARIES})
        cfw_add_test(cfw-platform X11InputTest)
        target_include_directories(X11InputTest PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(X11InputTest PRIVATE ${X11_LIBRARIES} ${CMAKE_DL_LIBS})
    endif()
endif()
