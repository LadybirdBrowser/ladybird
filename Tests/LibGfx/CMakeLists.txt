set(TEST_SOURCES
    BenchmarkJPEGLoader.cpp
    TestColor.cpp
    TestDeltaE.cpp
    TestICCProfile.cpp
    TestImageDecoder.cpp
    TestImageWriter.cpp
    TestMedianCut.cpp
    TestRect.cpp
    TestWOFF.cpp
    TestWOFF2.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibGfx LIBS LibGfx)
endforeach()
