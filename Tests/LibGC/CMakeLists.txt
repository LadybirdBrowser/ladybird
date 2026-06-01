set(TEST_SOURCES
    TestGCContainers.cpp
    TestGCIdleCollection.cpp
    TestGCVisitor.cpp
)

ladybird_test_suite(TestGC LibGC SOURCES ${TEST_SOURCES} LIBS LibGC)
