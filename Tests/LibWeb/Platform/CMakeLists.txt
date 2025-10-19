# Tests for LibWeb Platform components
# Includes tests for audio codec error handling and other platform abstractions

include(audio)

set(TEST_SOURCES
    TestAudioCodecErrorHandling.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    lagom_test("${source}" LibWeb LIBS LibWeb LibMedia LibFileSystem WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

# NOTE: Audio tests may require audio backend configuration
# The tests are designed to gracefully handle missing audio hardware
if (LADYBIRD_AUDIO_BACKEND STREQUAL "PULSE")
    target_compile_definitions(TestAudioCodecErrorHandling PRIVATE HAVE_PULSEAUDIO=1)
endif()
