# The Windows installer (docs/decisions/0017-windows-installer.md): one
# program that is the online installer, the offline installer (with a
# release package appended to it) and the maintenance tool. Built on CFW
# itself. Its logic builds and is tested on every platform; the installer
# is shipped for Windows.

add_library(cfw-installer-core STATIC
    src/Installer.cpp
    src/System.cpp
    src/Download.cpp
)
target_include_directories(cfw-installer-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cfw-installer-core PUBLIC cfw-io cfw-net)
target_compile_definitions(cfw-installer-core PUBLIC CFW_INSTALLER_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(cfw-installer-core PRIVATE advapi32)
endif()
cfw_configure_target(cfw-installer-core)

if(TARGET cfw-app)
    set(CFW_INSTALLER_SOURCES src/main.cpp src/Wizard.cpp)
    if(WIN32)
        # asInvoker: Windows would otherwise ask for administrator rights
        # for any program called "installer" (it installs per user).
        list(APPEND CFW_INSTALLER_SOURCES installer.rc)
    endif()
    add_executable(cfw-installer WIN32 ${CFW_INSTALLER_SOURCES})
    target_link_libraries(cfw-installer PRIVATE cfw-installer-core cfw-app)
    cfw_configure_target(cfw-installer)
    if(MINGW)
        # One file to download: no libstdc++/libgcc/winpthread DLLs, and no
        # debug symbols in the release build.
        target_link_options(cfw-installer PRIVATE -static $<$<CONFIG:Release>:-s>)
    endif()
    if(CFW_BUILD_TESTS)
        add_test(NAME InstallerEndToEnd COMMAND ${CMAKE_COMMAND}
            -DINSTALLER=$<TARGET_FILE:cfw-installer>
            -DPACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/testdata/ClannectFramework-0.9.0-windows-x64-mingw.zip
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/end-to-end
            "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/EndToEnd.cmake)
        set_tests_properties(InstallerEndToEnd PROPERTIES LABELS cfw-installer TIMEOUT 120)
        cfw_set_test_environment(InstallerEndToEnd)
        set_property(TEST InstallerEndToEnd APPEND PROPERTY ENVIRONMENT
            "CFW_INSTALLER_REGISTRY_SANDBOX=Software\\ClannectFrameworkInstallerTest")
    endif()
endif()

if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-installer-core InstallerTest)
    target_compile_definitions(InstallerTest PRIVATE CFW_INSTALLER_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    set_property(TEST InstallerTest APPEND PROPERTY ENVIRONMENT
        "CFW_INSTALLER_REGISTRY_SANDBOX=Software\\ClannectFrameworkInstallerTest")
endif()
