set(SOURCES
    DateTimeFormat.cpp
    DurationFormat.cpp
    DisplayNames.cpp
    ICU.cpp
    ListFormat.cpp
    Locale.cpp
    NumberFormat.cpp
    PluralRules.cpp
    RelativeTimeFormat.cpp
    UnicodeKeywords.cpp
)

serenity_lib(LibLocale locale)

find_package(ICU REQUIRED COMPONENTS data i18n uc)
target_include_directories(LibLocale PRIVATE ${ICU_INCLUDE_DIRS})
target_link_libraries(LibLocale PUBLIC ${ICU_LIBRARIES})
