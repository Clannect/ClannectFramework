# Fuzz targets for every untrusted-input parser (spec §8). Each target is
# built twice:
#
#   <Target>Replay  always: runs the committed corpus in corpus/<Target>/ as a
#                   CTest test, on every toolchain (MinGW included).
#   <Target>        with CFW_BUILD_FUZZERS=ON (Clang): a libFuzzer binary.
#                   Run it on the corpus directory; new inputs land there.
#
# See docs/decisions/0011-linux-ci-and-fuzzing.md.

function(cfw_add_fuzz_target name)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    set(corpus ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})

    if(CFW_BUILD_TESTS)
        add_executable(${name}Replay targets/${name}.cpp ReplayMain.cpp)
        target_link_libraries(${name}Replay PRIVATE ${ARG_DEPENDS})
        cfw_configure_target(${name}Replay)
        add_test(NAME ${name}Corpus COMMAND ${name}Replay ${corpus})
        set_tests_properties(${name}Corpus PROPERTIES LABELS fuzz TIMEOUT 60)
        cfw_set_test_environment(${name}Corpus)
    endif()

    if(CFW_BUILD_FUZZERS)
        add_executable(${name} targets/${name}.cpp)
        target_link_libraries(${name} PRIVATE ${ARG_DEPENDS})
        cfw_configure_target(${name})
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    endif()
endfunction()

cfw_add_fuzz_target(FuzzJson DEPENDS cfw-io)
cfw_add_fuzz_target(FuzzByteReader DEPENDS cfw-io)
cfw_add_fuzz_target(FuzzUrl DEPENDS cfw-core)
cfw_add_fuzz_target(FuzzWebSocketFrames DEPENDS cfw-net)
cfw_add_fuzz_target(FuzzHttp DEPENDS cfw-net)
cfw_add_fuzz_target(FuzzInflate DEPENDS cfw-core)
cfw_add_fuzz_target(FuzzPng DEPENDS cfw-image)
cfw_add_fuzz_target(FuzzJpeg DEPENDS cfw-image)
cfw_add_fuzz_target(FuzzWebP DEPENDS cfw-image)
cfw_add_fuzz_target(FuzzPaint DEPENDS cfw-gfx)
