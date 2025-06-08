if (NOT ENABLE_WEBGPUNATIVE)
    return()
endif()

set(TEST_SOURCES
    TestWebGPUNative.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibWebGPUNative LIBS LibWebGPUNative LibCore LibGfx)
endforeach()
