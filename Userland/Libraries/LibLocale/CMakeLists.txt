include(${SerenityOS_SOURCE_DIR}/Meta/CMake/locale_data.cmake)

if (DEFINED LOCALE_DATA_SOURCES)
    set(SOURCES ${LOCALE_DATA_SOURCES})
    set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})
    serenity_lib(LibLocaleData localedata TYPE OBJECT)
    target_compile_options(LibLocaleData PRIVATE -g0 -Os -Wno-parentheses-equality)
    target_link_libraries(LibLocaleData PRIVATE LibTimeZone)
    unset(GENERATED_SOURCES)
endif()

set(SOURCES
    DateTimeFormat.cpp
    Locale.cpp
    NumberFormat.cpp
    PluralRules.cpp
    RelativeTimeFormat.cpp
)

serenity_lib(LibLocale locale)
target_link_libraries(LibLocale PRIVATE LibUnicode)
target_compile_definitions(LibLocale PRIVATE ENABLE_UNICODE_DATA=$<BOOL:${ENABLE_UNICODE_DATABASE_DOWNLOAD}>)

find_package(ICU REQUIRED COMPONENTS data i18n uc)
target_include_directories(LibLocale PRIVATE ${ICU_INCLUDE_DIRS})
target_link_libraries(LibLocale PUBLIC ${ICU_LIBRARIES})

if (DEFINED LOCALE_DATA_SOURCES)
    target_link_libraries(LibLocale PRIVATE LibLocaleData)
endif()
