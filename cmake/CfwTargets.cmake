# Shared target setup for every CFW module and test.

# Warnings, debug contract checks, standard-library hardening and sanitizers.
function(cfw_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(CFW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # Not -Wnull-dereference: at -O3 GCC reports paths that a preceding
        # require() makes unreachable. UBSan and debug contracts cover it.
        # -Wshadow=local, not -Wshadow: GCC 13 flags scoped enumerators that
        # share a name with a type (VariantType::String vs cfw::String).
        # -Wno-missing-field-initializers: GCC 13 warns on designated
        # initialisers that omit fields with default member initialisers,
        # which is exactly how PropertyInfo{.name = ..., .type = ...} is meant
        # to be written.
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wno-missing-field-initializers)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wshadow=local)
        else()
            target_compile_options(${target} PRIVATE -Wshadow-uncaptured-local)
        endif()
        if(CFW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    # Debug builds check contracts and bounds-check every std container and
    # std::span (see docs/decisions/0003). PUBLIC so headers compiled into
    # dependants agree with the library about which checks are on.
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:CFW_DEBUG_CHECKS=1>
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:_GLIBCXX_ASSERTIONS=1>)

    # Coverage instrumentation for libFuzzer, on everything so the fuzzer sees
    # inside the modules. Only the fuzz binaries link the libFuzzer runtime.
    if(CFW_BUILD_FUZZERS)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    endif()

    foreach(sanitizer IN LISTS CFW_SANITIZE)
        if(sanitizer STREQUAL "ubsan-trap")
            target_compile_options(${target} PRIVATE -fsanitize=undefined -fsanitize-undefined-trap-on-error)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=${sanitizer} -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=${sanitizer})
            if(sanitizer STREQUAL "undefined")
                # Any UB report fails the test or fuzz run, instead of printing
                # and carrying on where nobody reads it.
                target_compile_options(${target} PRIVATE -fno-sanitize-recover=undefined)
            endif()
        endif()
    endforeach()
endfunction()

# cfw_add_module(cfw-core SOURCES ... DEPENDS ...)
# Every module is a static library with a public include/ directory and a
# cfw::<short-name> alias.
function(cfw_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    string(REPLACE "ce-" "" short_name ${name})
    add_library(cfw::${short_name} ALIAS ${name})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(${name} PUBLIC cxx_std_20)
    if(ARG_DEPENDS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    endif()
    cfw_configure_target(${name})
endfunction()

# cfw_add_test(cfw-core StringTest) builds tests/StringTest.cpp into its own
# binary and registers it with CTest.
function(cfw_add_test module test_name)
    if(NOT CFW_BUILD_TESTS)
        return()
    endif()
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE ${module} cfw-test-support)
    cfw_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    # §9 wants the unit tests fast; sanitizers slow them 2-15x (TSan most), so
    # the budget only applies to plain builds.
    set(timeout 10)
    if(CFW_SANITIZE)
        set(timeout 120)
    endif()
    set_tests_properties(${test_name} PROPERTIES LABELS ${module} TIMEOUT ${timeout})
endfunction()
