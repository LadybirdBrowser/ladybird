set(TEST_SOURCES
    TestThread.cpp
    TestThreadPool.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibThreading LIBS LibThreading)
endforeach()
