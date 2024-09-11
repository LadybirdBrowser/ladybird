set(TEST_SOURCES
    TestH264Decode.cpp
    TestParseMatroska.cpp
    TestPlaybackStream.cpp
    TestVorbisDecode.cpp
    TestVP9Decode.cpp
    TestWav.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibMedia LIBS LibMedia LibFileSystem WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

# The FLAC tests need a special working directory to find the test files
lagom_test(TestFLACSpec.cpp LIBS LibMedia WORKING_DIRECTORY "${FLAC_TEST_PATH}/..")

if (HAVE_PULSEAUDIO)
    target_compile_definitions(TestPlaybackStream PRIVATE HAVE_PULSEAUDIO=1)
endif()
