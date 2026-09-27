# cfw-gfx: the 2D renderer. Painter (backend-agnostic), pens, brushes, the
# stroker, and the CPU backend that paints into cfw::Image. Depends on
# cfw-image (Image, Rasterizer) and cfw-core; no third-party code
# (docs/decisions/0012, 0014).

cfw_add_module(cfw-gfx
    SOURCES
        src/Brush.cpp
        src/Painter.cpp
        src/RasterPaintBackend.cpp
        src/SvgImage.cpp
        src/Stroker.cpp
    DEPENDS
        cfw-image
        cfw-text
)

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-gfx StrokerTest)

    cfw_add_test(cfw-gfx PainterTest)

    cfw_add_test(cfw-gfx PainterGoldenTest)
    target_compile_definitions(PainterGoldenTest PRIVATE CFW_GFX_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
                                                         CFW_GFX_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(PainterGoldenTest PRIVATE cfw-io)

    cfw_add_test(cfw-gfx TextPaintTest)
    target_compile_definitions(TextPaintTest PRIVATE CFW_TEXT_FONTS="${PROJECT_SOURCE_DIR}/modules/cfw-text/testdata/fonts")
    target_link_libraries(TextPaintTest PRIVATE cfw-io)

    cfw_add_test(cfw-gfx SvgImageTest)
    target_compile_definitions(SvgImageTest PRIVATE CFW_GFX_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    target_link_libraries(SvgImageTest PRIVATE cfw-io)
endif()
