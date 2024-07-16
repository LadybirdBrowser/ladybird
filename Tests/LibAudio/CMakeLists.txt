set(TEST_SOURCES
    TestWav.cpp
    TestFLACSpec.cpp
    TestPlaybackStream.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibAudio LIBS LibAudio LibFileSystem)
endforeach()
