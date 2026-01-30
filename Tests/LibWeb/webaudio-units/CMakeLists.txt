set(WEBAUDIO_TEST_SOURCES
    TestAssociatedTaskQueue.cpp
    TestGraphCodecRoundTrip.cpp
    TestGraphCodecEncoder.cpp
    TestResampler.cpp
)

foreach(source IN LISTS WEBAUDIO_TEST_SOURCES)
    ladybird_test("${source}" LibWeb LIBS LibWeb LibThreading)
endforeach()

option(WEBAUDIO_STRESS_TESTS "Build WebAudio stress tests (slow and may require extra dependencies)" OFF)
if (WEBAUDIO_STRESS_TESTS)
    include(ffmpeg)
    ladybird_test("TestResamplerVsFFmpeg.cpp" LibWeb LIBS LibWeb NAME TestResamplerVsFFmpeg)
    target_link_libraries(TestResamplerVsFFmpeg PRIVATE PkgConfig::LIBSWRESAMPLE PkgConfig::AVUTIL)
endif()
