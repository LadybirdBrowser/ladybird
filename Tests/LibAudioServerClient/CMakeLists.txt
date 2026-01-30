set(TEST_SOURCES
    TestLibAudioServerClient.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibAudioServerClient LIBS LibAudioServerClient LibIPC)
endforeach()

if(NOT WIN32)
    add_dependencies(TestLibAudioServerClient AudioServer)
    target_link_libraries(TestLibAudioServerClient PRIVATE LibThreading)
endif()
