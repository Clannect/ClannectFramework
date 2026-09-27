# cfw-image: the RGBA8 Image type, size limits, and CFW's own codecs (PNG,
# JPEG, WebP), resizing and an atlas packer. Depends on cfw-core only; no
# third-party code and no OS calls (docs/decisions/0012).

cfw_add_module(cfw-image
    SOURCES
        src/Image.cpp
        src/JpegDecoder.cpp
        src/JpegEncoder.cpp
        src/Png.cpp
        src/WebP.cpp
        src/WebPLossless.cpp
        src/WebPLossy.cpp
    DEPENDS
        cfw-core
)

cfw_add_test(cfw-image PngTest)
target_compile_definitions(PngTest PRIVATE CFW_IMAGE_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
target_link_libraries(PngTest PRIVATE cfw-io)

cfw_add_test(cfw-image JpegTest)
target_compile_definitions(JpegTest PRIVATE CFW_IMAGE_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
target_link_libraries(JpegTest PRIVATE cfw-io)

cfw_add_test(cfw-image WebPTest)
target_compile_definitions(WebPTest PRIVATE CFW_IMAGE_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
target_link_libraries(WebPTest PRIVATE cfw-io)
