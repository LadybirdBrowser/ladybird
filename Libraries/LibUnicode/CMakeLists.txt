set(SOURCES
    Bidi.cpp
    Calendar.cpp
    Calendars/AdjustedEraCalendar.cpp
    Calendars/ChineseDangiCalendar.cpp
    CaseMapping.cpp
    CharacterTypes.cpp
    Collator.cpp
    CurrencyCode.cpp
    DateTimeFormat.cpp
    DateTimeFormatExports.cpp
    DisplayNames.cpp
    DurationFormat.cpp
    FullwidthMapping.cpp
    ICU.cpp
    IDNA.cpp
    IntlExports.cpp
    ListFormat.cpp
    Locale.cpp
    Normalize.cpp
    NumberFormat.cpp
    NumberFormatExports.cpp
    PluralRules.cpp
    RelativeTimeFormat.cpp
    RelativeTimeFormatExports.cpp
    Segmenter.cpp
    String.cpp
    TimeZone.cpp
    UnicodeKeywords.cpp
    Utf16String.cpp
)

set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

ladybird_lib(LibUnicode unicode)

target_link_libraries(LibUnicode PRIVATE ICU::i18n ICU::uc ICU::data)

string(REGEX MATCH "^[0-9]+" icu_version_major "${ICU_VERSION}")
if (CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(icu_data_file_name "icudt${icu_version_major}b.dat")
else()
    set(icu_data_file_name "icudt${icu_version_major}l.dat")
endif()
set(icu_data_file "${ICU_INCLUDE_DIR}/../share/icu/${icu_data_file_name}")
if (EXISTS "${icu_data_file}")
    cmake_path(NORMAL_PATH icu_data_file)

    target_sources(LibUnicode PRIVATE ICUData.cpp)
    target_link_libraries(LibUnicode PRIVATE ${CMAKE_DL_LIBS})

    # Nothing references the loader, whose constructor maps the data, so a static link has to be asked to take it.
    if (NOT BUILD_SHARED_LIBS)
        if (APPLE)
            target_link_options(LibUnicode INTERFACE "LINKER:-u,_ladybird_load_icu_data")
        elseif (WIN32)
            target_link_options(LibUnicode INTERFACE "LINKER:/INCLUDE:ladybird_load_icu_data")
        else()
            target_link_options(LibUnicode INTERFACE "LINKER:-u,ladybird_load_icu_data")
        endif()
    endif()

    set(icu_data_build_directory "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_DATADIR}/Lagom/icu")
    set(icu_data_build_file "${icu_data_build_directory}/${icu_data_file_name}")
    set(LADYBIRD_ICU_DATA_FILE "${icu_data_build_file}" CACHE INTERNAL "The ICU data file that ships with the resources")
    if (ICU_ICUPKG_EXECUTABLE AND NOT CMAKE_CROSSCOMPILING)
        set(trim_icu_data_script "${LADYBIRD_SOURCE_DIR}/Meta/CMake/trim_icu_data.cmake")
        add_custom_command(
            OUTPUT "${icu_data_build_file}"
            COMMAND "${CMAKE_COMMAND}"
                "-DICUPKG=${ICU_ICUPKG_EXECUTABLE}"
                "-DINPUT=${icu_data_file}"
                "-DOUTPUT=${icu_data_build_file}"
                "-DREMOVED_ITEMS_LIST=${CMAKE_CURRENT_BINARY_DIR}/icu_data_removed_items.txt"
                -P "${trim_icu_data_script}"
            DEPENDS "${icu_data_file}" "${trim_icu_data_script}"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT "${icu_data_build_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${icu_data_build_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${icu_data_file}" "${icu_data_build_file}"
            DEPENDS "${icu_data_file}"
            VERBATIM
        )
    endif()
    add_custom_target(copy_icu_data_to_build DEPENDS "${icu_data_build_file}")
    add_dependencies(LibUnicode copy_icu_data_to_build)
else()
    set(LADYBIRD_ICU_DATA_FILE "" CACHE INTERNAL "The ICU data file that ships with the resources")
endif()

import_rust_crate(MANIFEST_PATH Rust/Cargo.toml CRATE_NAME libunicode_rust FEATURES allocator idna FFI_HEADER RustFFI.h)
target_link_libraries(LibUnicode PRIVATE libunicode_rust)

# FIXME: Add support for building LibGfx in sanitize
#   lld-link: error: /failifmismatch: mismatch detected for 'annotate_string':
#   >>> lagom-unicode.lib(TimeZone.cpp.obj) has value 1
#   >>> lagom-gfx.lib(PaintingSurface.cpp.obj) has value 0
if (WIN32 AND ENABLE_ADDRESS_SANITIZER)
    target_compile_options(LibUnicode PRIVATE -fno-sanitize=address)
endif()
