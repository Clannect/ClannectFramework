# Fails if any Qt code or Qt library has found its way into CFW. Run by CTest
# (the NoQt test) on every build:
#
#   cmake -DROOT=<source dir> -DBINARY_DIR=<build dir> [-DOBJDUMP=<path>] -P CheckNoQt.cmake
#
# 1. Sources and build files: no Qt headers, macros, CMake packages or targets.
# 2. Built binaries: no Qt shared library among their imports (needs objdump).
#
# Documentation may talk about Qt (it is what CFW replaces), so docs/ and
# Markdown files are not scanned. This file is excluded because it has to
# spell the patterns out.

cmake_minimum_required(VERSION 3.25)

if(NOT ROOT OR NOT BINARY_DIR)
    message(FATAL_ERROR "CheckNoQt.cmake needs -DROOT and -DBINARY_DIR")
endif()

set(violations "")

# Patterns are built from pieces so this file's own text never matches them.
set(q "Q")
set(source_patterns
    "#[ \t]*include[ \t]*[<\"]${q}[A-Za-z]"   # #include <QString>, <QtCore/...>
    "${q}_OBJECT|${q}_PROPERTY|${q}_SIGNALS|${q}_SLOTS|${q}_EMIT|${q}_INVOKABLE"
    "${q}StringLiteral|${q}Latin1String"
    "\\b${q}t[56]::"                           # Qt6::Widgets
    "find_package[ \t]*\\([ \t]*${q}t"         # find_package(Qt6 ...)
    "\\bqt_(add|standard|wrap|finalize)"       # qt_add_executable, qt_standard_project_setup
    "CMAKE_AUTO(MOC|UIC|RCC)[ \t]+ON"
)

string(TIMESTAMP started "%s")

# Text files only: test data (images, fuzz inputs) is not code, and reading
# megabytes of it line by line made this check slow.
set(text_extensions h hpp hxx c cc cpp cxx inl cmake txt json py sh ps1 yml yaml in)
file(GLOB_RECURSE candidates
    "${ROOT}/modules/*" "${ROOT}/testing/*" "${ROOT}/cmake/*" "${ROOT}/tools/*" "${ROOT}/fuzz/*.cpp"
    "${ROOT}/fuzz/CMakeLists.txt" "${ROOT}/bench/*.cpp" "${ROOT}/bench/*.h" "${ROOT}/bench/CMakeLists.txt"
    "${ROOT}/CMakeLists.txt" "${ROOT}/CMakePresets.json")
set(scanned 0)
foreach(path IN LISTS candidates)
    if(path MATCHES "CheckNoQt\\.cmake$" OR path MATCHES "/testdata/" OR path MATCHES "/qt-oracle/")
        continue()
    endif()
    get_filename_component(name "${path}" NAME)
    get_filename_component(extension "${path}" LAST_EXT)
    string(REPLACE "." "" extension "${extension}")
    if(NOT name STREQUAL "CMakeLists.txt" AND NOT extension IN_LIST text_extensions)
        continue()
    endif()
    math(EXPR scanned "${scanned} + 1")
    file(STRINGS "${path}" lines)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        foreach(pattern IN LISTS source_patterns)
            if(line MATCHES "${pattern}")
                string(APPEND violations "  ${path}:${line_number}: ${line}\n")
            endif()
        endforeach()
    endforeach()
endforeach()

string(TIMESTAMP now "%s")
math(EXPR elapsed "${now} - ${started}")
message(STATUS "NoQt: scanned ${scanned} source and build files in ${elapsed} s")

# Binaries: every executable and shared library in the build tree.
if(OBJDUMP AND EXISTS "${OBJDUMP}")
    file(GLOB_RECURSE binaries "${BINARY_DIR}/*.exe" "${BINARY_DIR}/*.dll" "${BINARY_DIR}/*.so")
    list(FILTER binaries EXCLUDE REGEX "/CMakeFiles/")
    foreach(binary IN LISTS binaries)
        execute_process(COMMAND "${OBJDUMP}" -p "${binary}" OUTPUT_VARIABLE dump ERROR_QUIET
                        RESULT_VARIABLE result TIMEOUT 30)
        if(NOT result EQUAL 0)
            string(APPEND violations "  ${binary}: ${OBJDUMP} -p failed (${result}); cannot check its imports\n")
        endif()
        string(REGEX MATCHALL "(DLL Name|NEEDED)[: \t]+[^\n]*${q}t[56][^\n]*" hits "${dump}")
        foreach(hit IN LISTS hits)
            string(APPEND violations "  ${binary}: imports ${hit}\n")
        endforeach()
    endforeach()
    list(LENGTH binaries binary_count)
    string(TIMESTAMP now "%s")
    math(EXPR elapsed "${now} - ${started}")
    message(STATUS "NoQt: checked imports of ${binary_count} binaries (${elapsed} s in all)")
else()
    message(STATUS "NoQt: objdump not found; binary import check skipped")
endif()

if(violations)
    message(FATAL_ERROR "Qt found in Clannect Framework:\n${violations}")
endif()
message(STATUS "NoQt: clean")
