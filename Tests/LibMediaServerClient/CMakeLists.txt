set(TEST_SOURCES
    TestLibMediaServerClient.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibMediaServerClient LIBS LibMediaServerClient LibIPC)
endforeach()

if(NOT WIN32)
    target_link_libraries(TestLibMediaServerClient PRIVATE LibThreading)
endif()
