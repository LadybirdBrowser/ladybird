set(TEST_SOURCES
    TestTimeZoneDeprecated.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibTimeZone LIBS LibCore LibTimeZone)

    get_filename_component(target "${source}" NAME_WLE)
    target_compile_definitions("${target}" PRIVATE ENABLE_TIME_ZONE_DATA=$<BOOL:${ENABLE_TIME_ZONE_DATABASE_DOWNLOAD}>)
endforeach()
