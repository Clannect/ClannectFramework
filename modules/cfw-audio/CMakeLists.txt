# cfw-audio: sound output and CFW's own decoders (WAV, Ogg Vorbis, MP3, FLAC)
# and resampler. Depends on cfw-core only; no third-party code.
#
# The decoders and the resampler make no OS calls. The output device is the
# system's: WASAPI on Windows, CoreAudio on macOS, and on Linux ALSA loaded
# at run time (nothing to link but dlopen), as cfw-net does with OpenSSL.

set(CFW_AUDIO_PLATFORM_SOURCES)
set(CFW_AUDIO_PLATFORM_LIBS)
if(WIN32)
    list(APPEND CFW_AUDIO_PLATFORM_SOURCES src/AudioDeviceWasapi.cpp)
    list(APPEND CFW_AUDIO_PLATFORM_LIBS ole32)
elseif(APPLE)
    list(APPEND CFW_AUDIO_PLATFORM_SOURCES src/AudioDeviceCoreAudio.cpp)
    list(APPEND CFW_AUDIO_PLATFORM_LIBS "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CFW_AUDIO_PLATFORM_SOURCES src/AudioDeviceAlsa.cpp)
    list(APPEND CFW_AUDIO_PLATFORM_LIBS ${CMAKE_DL_LIBS})
else()
    list(APPEND CFW_AUDIO_PLATFORM_SOURCES src/AudioDeviceNone.cpp)
endif()

find_package(Threads REQUIRED)

cfw_add_module(cfw-audio
    SOURCES
        src/AudioDecoder.cpp
        src/AudioDevice.cpp
        src/Flac.cpp
        src/Mp3.cpp
        src/Resampler.cpp
        src/Vorbis.cpp
        src/Wav.cpp
        ${CFW_AUDIO_PLATFORM_SOURCES}
    DEPENDS
        cfw-core
)
target_link_libraries(cfw-audio PRIVATE ${CFW_AUDIO_PLATFORM_LIBS} Threads::Threads)

# Tests (only when CFW builds them: not as a subproject).
if(CFW_BUILD_TESTS)
    foreach(test WavTest FlacTest VorbisTest Mp3Test)
        cfw_add_test(cfw-audio ${test})
        target_compile_definitions(${test} PRIVATE CFW_AUDIO_TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
        target_link_libraries(${test} PRIVATE cfw-io)
    endforeach()
    cfw_add_test(cfw-audio ResamplerTest)
    cfw_add_test(cfw-audio AudioDeviceTest)
    target_link_libraries(AudioDeviceTest PRIVATE Threads::Threads)
endif()
