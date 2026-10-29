ladybird_test_suite(LibDatabase)

set(TEST_SOURCES
    TestDatabase.cpp
    TestMigrations.cpp
)

target_sources(TestLibDatabase PRIVATE ${TEST_SOURCES})
target_link_libraries(TestLibDatabase PRIVATE LibCore LibDatabase LibFileSystem)
