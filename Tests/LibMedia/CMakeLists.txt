include(audio)

set(TEST_SOURCES
    TestAudioDataProvider.cpp
    TestAudioMixingSink.cpp
    TestCICP.cpp
    TestDataProviders.cpp
    TestH264Decode.cpp
    TestIncrementallyPopulatedStream.cpp
    TestMatroskaDemuxer.cpp
    TestParseMatroska.cpp
    TestPlaybackManager.cpp
    TestVorbisDecode.cpp
    TestVP9Decode.cpp
    TestWav.cpp
)

if (NOT DEFINED LADYBIRD_AUDIO_BACKEND)
    list(REMOVE_ITEM TEST_SOURCES TestAudioMixingSink.cpp)
endif()

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibMedia LIBS LibMedia LibFileSystem WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

target_link_libraries(TestIncrementallyPopulatedStream PRIVATE LibThreading)
if (TARGET TestAudioMixingSink)
    target_link_libraries(TestAudioMixingSink PRIVATE LibAudioServer LibIPC)
endif()
