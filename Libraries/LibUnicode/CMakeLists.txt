set(SOURCES
    CharacterTypes.cpp
    Collator.cpp
    CurrencyCode.cpp
    DateTimeFormat.cpp
    DisplayNames.cpp
    DurationFormat.cpp
    ICU.cpp
    IDNA.cpp
    ListFormat.cpp
    Locale.cpp
    Normalize.cpp
    NumberFormat.cpp
    PluralRules.cpp
    RelativeTimeFormat.cpp
    Segmenter.cpp
    String.cpp
    TimeZone.cpp
    UnicodeKeywords.cpp
)

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

serenity_lib(LibUnicode unicode)

find_package(ICU 76 REQUIRED COMPONENTS data i18n uc)
target_link_libraries(LibUnicode PRIVATE ICU::i18n ICU::uc ICU::data)
