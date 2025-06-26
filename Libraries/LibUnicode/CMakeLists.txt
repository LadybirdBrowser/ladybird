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
    Utf16String.cpp
)

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

ladybird_lib(LibUnicode unicode)

find_package(ICU 76 REQUIRED COMPONENTS data i18n uc)
target_link_libraries(LibUnicode PRIVATE ICU::i18n ICU::uc ICU::data)

# FIXME: Add support for building LibGfx in sanitize
#   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
#   >>> lagom-unicode.lib(TimeZone.cpp.obj) has value 1
#   >>> lagom-gfx.lib(PaintingSurface.cpp.obj) has value 0
if (WIN32 AND ENABLE_WINDOWS_CI)
    target_compile_options(LibUnicode PRIVATE -fno-sanitize=address)
endif()
