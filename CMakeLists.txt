cmake_minimum_required(VERSION 3.25)

project(ClannectFramework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CFW_BUILD_TESTS "Build the per-module unit tests" ON)
option(CFW_WARNINGS_AS_ERRORS "Treat compiler warnings in CFW's own code as errors" ON)
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
        -DROOT=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR} -DOBJDUMP=${CFW_OBJDUMP}
        -P ${CMAKE_SOURCE_DIR}/cmake/CheckNoQt.cmake)
    set_tests_properties(NoQt PROPERTIES LABELS policy)
endif()

# Lower layers first. A module may only link modules listed above it.
add_subdirectory(modules/cfw-core)
add_subdirectory(modules/cfw-io)
add_subdirectory(modules/cfw-net)

add_subdirectory(bench)
