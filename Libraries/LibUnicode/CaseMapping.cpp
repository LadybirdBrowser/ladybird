/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/ICU.h>
#include <LibUnicode/TextMapping.h>
#include <unicode/casemap.h>
#include <unicode/edits.h>
#include <unicode/stringoptions.h>

namespace Unicode {

static void apply_case_mapping_to(Utf16View const& string, CaseMapping mapping, Optional<Utf16View> const& locale, bool preserve_existing, UnicodeTextMappingOutput output)
{
    auto icu_source = icu_string(string);
    auto const* source = reinterpret_cast<char16_t const*>(icu_source.getBuffer());
    auto const source_length = icu_source.length();

    char const* locale_name = nullptr;
    if (locale.has_value()) {
        auto locale_string = MUST(locale->to_utf8(AllowLonelySurrogates::Yes));
        if (auto locale_data = LocaleData::for_locale(locale_string.bytes_as_string_view()); locale_data.has_value())
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
            if (preserve_existing)
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
    auto* destination = reinterpret_cast<char16_t*>(output.allocate_text(output.context, destination_length));
    icu::Edits icu_edits;
    VERIFY(map(destination, destination_length, &icu_edits, status) == destination_length);
    verify_icu_success(status);

    auto iterator = icu_edits.getFineChangesIterator();
    while (iterator.next(status)) {
        if (iterator.oldLength() == iterator.newLength())
            continue;
        output.append_edit(output.context,
            static_cast<size_t>(iterator.sourceIndex()),
            static_cast<size_t>(iterator.oldLength()),
            static_cast<size_t>(iterator.destinationIndex()),
            static_cast<size_t>(iterator.newLength()));
    }
    verify_icu_success(status);
}

}

extern "C" void unicode_apply_case_mapping(u16 const* text, size_t length, u8 mapping, u16 const* locale, size_t locale_length, bool preserve_existing, UnicodeTextMappingOutput output)
{
    VERIFY(mapping <= to_underlying(Unicode::CaseMapping::Titlecase));
    auto locale_view = locale ? Optional<Utf16View> { Utf16View { reinterpret_cast<char16_t const*>(locale), locale_length } } : Optional<Utf16View> {};
    Unicode::apply_case_mapping_to({ reinterpret_cast<char16_t const*>(text), length }, static_cast<Unicode::CaseMapping>(mapping), locale_view,
        preserve_existing, output);
}
