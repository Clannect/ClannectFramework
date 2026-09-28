# cfw-core: strings, containers, Variant, Result, signals, math.
# Zero third-party dependencies and zero OS calls: it must build for the
# headless Runtime and for every platform unchanged.

cfw_add_module(cfw-core
    SOURCES
        src/Arena.cpp
        src/Base64.cpp
        src/Checksum.cpp
        src/Clock.cpp
        src/CommandLine.cpp
        src/ClassInfo.cpp
        src/Color.cpp
        src/Contract.cpp
        src/Deflate.cpp
        src/Error.cpp
        src/Locale.cpp
        src/Logger.cpp
        src/Mat3.cpp
        src/Mat4.cpp
        src/PainterPath.cpp
        src/PropertyBag.cpp
        src/Quat.cpp
        src/Sha1.cpp
        src/Sha256.cpp
        src/Strings.cpp
        src/Transform2D.cpp
        src/Url.cpp
        src/Utf8.cpp
        src/Uuid.cpp
        src/Variant.cpp
)

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    cfw_add_test(cfw-core ResultTest)
    cfw_add_test(cfw-core DeflateTest)
    cfw_add_test(cfw-core LocaleTest)
    cfw_add_test(cfw-core Utf8Test)
    cfw_add_test(cfw-core StringsTest)
    cfw_add_test(cfw-core NameTest)
    cfw_add_test(cfw-core SmallVectorTest)
    cfw_add_test(cfw-core FlatMapTest)
    cfw_add_test(cfw-core MathTest)
    cfw_add_test(cfw-core ColorTest)
    cfw_add_test(cfw-core RotationTest)
    cfw_add_test(cfw-core GeometryTest)
    cfw_add_test(cfw-core VariantTest)
    cfw_add_test(cfw-core SignalTest)
    cfw_add_test(cfw-core PropertyTest)
    cfw_add_test(cfw-core EncodingTest)
    cfw_add_test(cfw-core RuntimeSupportTest)
    cfw_add_test(cfw-core ResourceEmbedTest)
    cfw_embed_resources(ResourceEmbedTest FUNCTION testResource NAMESPACE cfwtest BASE tests/data
                        FILES tests/data/embedded/note.txt tests/data/embedded/random.bin)
    target_compile_definitions(ResourceEmbedTest PRIVATE
        CFW_EMBED_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
