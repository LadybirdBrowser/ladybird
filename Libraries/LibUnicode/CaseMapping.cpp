/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/CaseMapping.h>
#include <LibUnicode/ICU.h>
#include <unicode/casemap.h>
#include <unicode/edits.h>
#include <unicode/stringoptions.h>

namespace Unicode {

CaseMappingResult apply_case_mapping(Utf16String const& string, CaseMapping mapping, Optional<Utf16View> const& locale, TrailingCodePointTransformation trailing_code_point_transformation)
{
    if (string.has_ascii_storage() && !locale.has_value()) {
        switch (mapping) {
        case CaseMapping::Lowercase:
            return { string.to_ascii_lowercase(), {} };
        case CaseMapping::Uppercase:
            return { string.to_ascii_uppercase(), {} };
        case CaseMapping::Titlecase:
            break;
        }
    }

    auto icu_source = icu_string(string);
    auto const* source = reinterpret_cast<char16_t const*>(icu_source.getBuffer());
    auto const source_length = icu_source.length();

    char const* locale_name = nullptr;
    if (locale.has_value()) {
        if (auto locale_data = LocaleData::for_locale(locale->bytes()); locale_data.has_value())
            locale_name = locale_data->locale().getName();
    }

    auto map = [&](char16_t* destination, i32 destination_capacity, icu::Edits* edits, UErrorCode& status) {
        switch (mapping) {
        case CaseMapping::Lowercase:
            return icu::CaseMap::toLower(locale_name, 0, source, source_length, destination, destination_capacity, edits, status);
        case CaseMapping::Uppercase:
            return icu::CaseMap::toUpper(locale_name, 0, source, source_length, destination, destination_capacity, edits, status);
        case CaseMapping::Titlecase: {
            u32 options = 0;
            if (trailing_code_point_transformation == TrailingCodePointTransformation::PreserveExisting)
                options |= U_TITLECASE_NO_LOWERCASE;
            return icu::CaseMap::toTitle(locale_name, options, nullptr, source, source_length, destination, destination_capacity, edits, status);
        }
        }
        VERIFY_NOT_REACHED();
    };

    UErrorCode status = U_ZERO_ERROR;
    auto const destination_length = map(nullptr, 0, nullptr, status);
    if (status != U_BUFFER_OVERFLOW_ERROR)
        verify_icu_success(status);

    status = U_ZERO_ERROR;
    Vector<char16_t> destination;
    destination.resize(destination_length);
    icu::Edits icu_edits;
    VERIFY(map(destination.data(), destination_length, &icu_edits, status) == destination_length);
    verify_icu_success(status);

    Vector<CaseMappingEdit> edits;
    auto iterator = icu_edits.getFineChangesIterator();
    while (iterator.next(status)) {
        if (iterator.oldLength() == iterator.newLength())
            continue;
        edits.append({
            static_cast<size_t>(iterator.sourceIndex()),
            static_cast<size_t>(iterator.oldLength()),
            static_cast<size_t>(iterator.destinationIndex()),
            static_cast<size_t>(iterator.newLength()),
        });
    }
    verify_icu_success(status);

    auto text = destination.is_empty()
        ? Utf16String {}
        : Utf16String::from_utf16({ destination.data(), destination.size() });
    return { move(text), move(edits) };
}

}
