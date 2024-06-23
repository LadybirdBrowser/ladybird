include(${SerenityOS_SOURCE_DIR}/Meta/CMake/time_zone_data.cmake)

set(SOURCES
    ${TIME_ZONE_DATA_SOURCES}
    TimeZone.cpp
)

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

serenity_lib(LibTimeZone timezone)
target_compile_definitions(LibTimeZone PRIVATE ENABLE_TIME_ZONE_DATA=$<BOOL:${ENABLE_TIME_ZONE_DATABASE_DOWNLOAD}>)
