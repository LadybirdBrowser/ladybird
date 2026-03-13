set(SOURCES
    Calendar.cpp
    Calendars/AdjustedEraCalendar.cpp
    Calendars/ChineseDangiCalendar.cpp
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

find_package(ICU 78.2 EXACT REQUIRED COMPONENTS data i18n uc)
target_link_libraries(LibUnicode PRIVATE ICU::i18n ICU::uc ICU::data)

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libunicode_rust)
target_link_libraries(LibUnicode PRIVATE libunicode_rust)

# FIXME: Add support for building LibGfx in sanitize
#   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
#   >>> lagom-unicode.lib(TimeZone.cpp.obj) has value 1
#   >>> lagom-gfx.lib(PaintingSurface.cpp.obj) has value 0
if (WIN32 AND ENABLE_ADDRESS_SANITIZER)
    target_compile_options(LibUnicode PRIVATE -fno-sanitize=address)
endif()
