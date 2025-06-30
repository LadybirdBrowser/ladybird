set(TEST_SOURCES
    TestColor.cpp
    TestImageWriter.cpp
    TestQuad.cpp
    TestRect.cpp
)

# FIXME: Address runtime errors for file-based tests on Windows
if (NOT WIN32)
    list(APPEND TEST_SOURCES
        BenchmarkJPEGLoader.cpp
        TestImageDecoder.cpp
        TestWOFF.cpp
        TestWOFF2.cpp
    )
endif()

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibGfx LIBS LibGfx)
endforeach()
