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

# Windows builds run on Linux under Wine (cmake/toolchains/mingw-w64-cross.cmake):
# Wine needs a UTF-8 locale to map non-ASCII file names, and tests use
# CFW_UNDER_WINE to report what Wine cannot do instead of failing on it.
function(cfw_set_test_environment test_name)
    if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR MATCHES "wine")
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "CFW_UNDER_WINE=1;LANG=C.UTF-8;LC_ALL=C.UTF-8;WINEDEBUG=-all")
    endif()
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
    cfw_set_test_environment(${test_name})
endfunction()

# Files built into a target (Qt's .qrc): at configure time each file becomes
# a byte array, and a generated header declares
#
#     cfw::Span<const std::byte> <FUNCTION>(cfw::StringView path);
#
# in `NAMESPACE`, which returns a file's bytes by its path relative to BASE
# (an empty span for anything else). Include it as "<FUNCTION>.h". Editing an
# embedded file re-runs the configure step.
#
#   cfw_embed_resources(app FUNCTION appResource NAMESPACE app BASE resources
#                       FILES resources/brand/logo.png)
function(cfw_embed_resources target)
    cmake_parse_arguments(ARG "" "FUNCTION;NAMESPACE;BASE" "FILES" ${ARGN})
    if(NOT ARG_FUNCTION OR NOT ARG_NAMESPACE OR NOT ARG_FILES)
        message(FATAL_ERROR "cfw_embed_resources: FUNCTION, NAMESPACE and FILES are required")
    endif()
    if(NOT ARG_BASE)
        set(ARG_BASE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    get_filename_component(base "${ARG_BASE}" ABSOLUTE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/cfw_resources/${target}")
    file(MAKE_DIRECTORY "${out_dir}")

    set(arrays "")
    set(entries "")
    set(index 0)
    foreach(file IN LISTS ARG_FILES)
        get_filename_component(absolute "${file}" ABSOLUTE)
        file(RELATIVE_PATH relative "${base}" "${absolute}")
        file(READ "${absolute}" hex HEX)
        string(LENGTH "${hex}" hex_length)
        math(EXPR size "${hex_length} / 2")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        # Break the initialiser into lines of 32 bytes.
        string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
        string(APPEND arrays "alignas(16) const unsigned char kFile${index}[${size} + 1] = {\n${bytes}0};\n")
        string(APPEND entries "    {\"${relative}\", kFile${index}, ${size}},\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${absolute}")
        math(EXPR index "${index} + 1")
    endforeach()

    set(header "// Generated by cfw_embed_resources; do not edit.\n#pragma once\n\n#include <cstddef>\n\n#include \"cfw/core/Span.h\"\n#include \"cfw/core/String.h\"\n\nnamespace ${ARG_NAMESPACE} {\n\n// An embedded file's bytes by its path; empty if there is none.\ncfw::Span<const std::byte> ${ARG_FUNCTION}(cfw::StringView path);\n\n} // namespace ${ARG_NAMESPACE}\n")
    set(source "// Generated by cfw_embed_resources; do not edit.\n#include \"${ARG_FUNCTION}.h\"\n\nnamespace ${ARG_NAMESPACE} {\n\nnamespace {\n\n${arrays}\nstruct Entry {\n    const char *path;\n    const unsigned char *data;\n    std::size_t size;\n};\n\nconst Entry kEntries[] = {\n${entries}};\n\n} // namespace\n\ncfw::Span<const std::byte> ${ARG_FUNCTION}(cfw::StringView path) {\n    for (const Entry &entry : kEntries) {\n        if (path == entry.path) {\n            return {reinterpret_cast<const std::byte *>(entry.data), entry.size};\n        }\n    }\n    return {};\n}\n\n} // namespace ${ARG_NAMESPACE}\n")
    # Rewritten only when the content changes, so builds stay incremental.
    file(WRITE "${out_dir}/${ARG_FUNCTION}.h.tmp" "${header}")
    configure_file("${out_dir}/${ARG_FUNCTION}.h.tmp" "${out_dir}/${ARG_FUNCTION}.h" COPYONLY)
    file(WRITE "${out_dir}/${ARG_FUNCTION}.cpp.tmp" "${source}")
    configure_file("${out_dir}/${ARG_FUNCTION}.cpp.tmp" "${out_dir}/${ARG_FUNCTION}.cpp" COPYONLY)
    target_sources(${target} PRIVATE "${out_dir}/${ARG_FUNCTION}.cpp")
    target_include_directories(${target} PRIVATE "${out_dir}")
    target_link_libraries(${target} PRIVATE cfw-core)
endfunction()

# Objective-C++ sources (the macOS backends): ARC, and none of the C++-only
# style warnings AppKit's idioms trip (bridged casts are C-style casts,
# delegate methods ignore most of their parameters).
function(cfw_objcxx_sources)
    set_source_files_properties(${ARGN} PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc;-Wno-old-style-cast;-Wno-pedantic;-Wno-deprecated-declarations;-Wno-unused-parameter")
endfunction()
