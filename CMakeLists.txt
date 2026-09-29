cmake_minimum_required(VERSION 3.25)

# macOS 13.3: the first whose libc++ has floating-point std::to_chars.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

project(ClannectFramework VERSION 0.1.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX) # the Cocoa backends (*.mm)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# As a subproject (e.g. ClannectEngine's add_subdirectory/FetchContent), only
# the libraries build: tests, benchmarks and fuzzers are CFW's own business.
option(CFW_BUILD_TESTS "Build the per-module unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CFW_BUILD_TOOLS "Build the benchmarks and fuzz targets" ${PROJECT_IS_TOP_LEVEL})
option(CFW_BUILD_FUZZERS "Build libFuzzer binaries for every parser (Clang only; see fuzz/)" OFF)
option(CFW_WARNINGS_AS_ERRORS "Treat compiler warnings in CFW's own code as errors" ${PROJECT_IS_TOP_LEVEL})
set(CFW_SANITIZE "" CACHE STRING
    "Sanitizers: a list of address, undefined, thread, or ubsan-trap (UBSan without a runtime; works on MinGW)")

include(cmake/CfwTargets.cmake)

if(CFW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(testing)

    # CFW exists to remove Qt. This test fails the build if any Qt code or Qt
    # library appears in sources, build files or linked binaries.
    find_program(CFW_OBJDUMP NAMES objdump llvm-objdump)
    add_test(NAME NoQt COMMAND ${CMAKE_COMMAND}
        -DROOT=${PROJECT_SOURCE_DIR} -DBINARY_DIR=${PROJECT_BINARY_DIR} -DOBJDUMP=${CFW_OBJDUMP}
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckNoQt.cmake)
    # Its own timeout, so the CI's global --timeout for Wine runs does not apply.
    set_tests_properties(NoQt PROPERTIES LABELS policy TIMEOUT 600)
endif()

# Lower layers first. A module may only link modules listed above it.
add_subdirectory(modules/cfw-core)
add_subdirectory(modules/cfw-io)
add_subdirectory(modules/cfw-net)
add_subdirectory(modules/cfw-image)
add_subdirectory(modules/cfw-text)
add_subdirectory(modules/cfw-gfx)
add_subdirectory(modules/cfw-platform)
add_subdirectory(modules/cfw-ui)
add_subdirectory(modules/cfw-app)

if(CFW_BUILD_TOOLS)
    add_subdirectory(fuzz)
endif()
if(CFW_BUILD_TESTS)
    add_subdirectory(testing/tls-interop)
endif()
if(CFW_BUILD_TOOLS)
    add_subdirectory(bench)
    add_subdirectory(examples)
endif()
