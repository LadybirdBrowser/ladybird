set(TEST_SOURCES
    TestDatabase.cpp
    TestMigrations.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" LibDatabase LIBS LibCore LibDatabase LibFileSystem)
endforeach()
