# cfw-io: paths, files, atomic save, directories, standard locations, file
# watching, settings, JSON and binary serialisation. Depends on cfw-core only;
# platform code lives in the *Win32.cpp / *Posix.cpp sources.

set(CFW_IO_PLATFORM_SOURCES)
set(CFW_IO_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_IO_PLATFORM_SOURCES src/PlatformFileWin32.cpp src/StandardPathsWin32.cpp)
    list(APPEND CFW_IO_PLATFORM_LIBS shell32 ole32)
else()
    list(APPEND CFW_IO_PLATFORM_SOURCES src/PlatformFilePosix.cpp src/StandardPathsPosix.cpp)
endif()

cfw_add_module(cfw-io
    SOURCES
        src/ByteBuffer.cpp
        src/FileSystem.cpp
        src/FileWatcher.cpp
        src/Json.cpp
        src/JsonReader.cpp
        src/JsonWriter.cpp
        src/MappedFile.cpp
        src/Path.cpp
        src/Settings.cpp
        src/TemporaryDirectory.cpp
        ${CFW_IO_PLATFORM_SOURCES}
    DEPENDS
        cfw-core
)
target_link_libraries(cfw-io PRIVATE ${CFW_IO_PLATFORM_LIBS})

cfw_add_test(cfw-io JsonTest)
cfw_add_test(cfw-io ByteBufferTest)
cfw_add_test(cfw-io FileSystemTest)
target_compile_definitions(JsonTest PRIVATE CFW_IO_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
cfw_add_test(cfw-io FuzzSmokeTest)
target_compile_definitions(FuzzSmokeTest PRIVATE CFW_IO_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
