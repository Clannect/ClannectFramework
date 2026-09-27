# cfw-text: Unicode properties and algorithms (grapheme clusters, line
# breaking, bidi), font files, shaping, layout and the glyph cache. Everything
# is CFW's own (docs/decisions/0012, 0015): no FreeType, HarfBuzz or ICU.
# Depends on cfw-image (Rasterizer, RectPacker) and cfw-io (font files).

cfw_add_module(cfw-text
    SOURCES
        src/Bidi.cpp
        src/FontFace.cpp
        src/OtLayout.cpp
        src/Shaper.cpp
        src/TextBreaks.cpp
        src/Unicode.cpp
    DEPENDS
        cfw-image
        cfw-io
)

cfw_add_test(cfw-text TextBreaksTest)
target_compile_definitions(TextBreaksTest PRIVATE CFW_TEXT_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# One executable, two tests (one per conformance file), so each stays within
# the time budget.
if(CFW_BUILD_TESTS)
    add_executable(BidiTest tests/BidiTest.cpp)
    target_link_libraries(BidiTest PRIVATE cfw-text cfw-test-support)
    target_compile_definitions(BidiTest PRIVATE CFW_TEXT_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    cfw_configure_target(BidiTest)
endif()
foreach(part classes characters)
    if(NOT CFW_BUILD_TESTS)
        break()
    endif()
    add_test(NAME BidiTest-${part} COMMAND BidiTest ${part})
    set_tests_properties(BidiTest-${part} PROPERTIES LABELS cfw-text TIMEOUT 10)
    if(CFW_SANITIZE)
        set_tests_properties(BidiTest-${part} PROPERTIES TIMEOUT 120)
    endif()
    cfw_set_test_environment(BidiTest-${part})
endforeach()

cfw_add_test(cfw-text FontFaceTest)
target_compile_definitions(FontFaceTest PRIVATE CFW_TEXT_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

cfw_add_test(cfw-text ShaperTest)
target_compile_definitions(ShaperTest PRIVATE CFW_TEXT_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
