/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Concepts.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>

#include <simdutf.h>

namespace AK {

bool validate_utf16_le(ReadonlyBytes bytes)
{
    return simdutf::validate_utf16le(reinterpret_cast<char16_t const*>(bytes.data()), bytes.size() / 2);
}

bool validate_utf16_be(ReadonlyBytes bytes)
{
    return simdutf::validate_utf16be(reinterpret_cast<char16_t const*>(bytes.data()), bytes.size() / 2);
}

size_t utf16_code_unit_length_from_utf8(StringView string)
{
    return simdutf::utf16_length_from_utf8(string.characters_without_null_termination(), string.length());
}

ErrorOr<String> Utf16View::to_utf8(AllowLonelySurrogates allow_lonely_surrogates) const
{
    if (is_empty())
        return String {};
    if (has_ascii_storage())
        return String::from_utf8_without_validation(bytes());

    if (!validate(allow_lonely_surrogates))
        return Error::from_string_literal("Input was not valid UTF-16");

    if (allow_lonely_surrogates == AllowLonelySurrogates::No) {
        String result;

        auto utf8_length = simdutf::utf8_length_from_utf16(m_string.utf16, length_in_code_units());

        TRY(result.replace_with_new_string(Badge<Utf16View> {}, utf8_length, [&](Bytes buffer) -> ErrorOr<void> {
            [[maybe_unused]] auto result = simdutf::convert_utf16_to_utf8(m_string.utf16, length_in_code_units(), reinterpret_cast<char*>(buffer.data()));
            ASSERT(result == buffer.size());
            return {};
        }));

        return result;
    }

    StringBuilder builder;
    builder.append(*this);
    return builder.to_string();
}

ErrorOr<ByteString> Utf16View::to_byte_string(AllowLonelySurrogates allow_lonely_surrogates) const
{
    return TRY(to_utf8(allow_lonely_surrogates)).to_byte_string();
}

Utf16String Utf16View::to_ascii_lowercase() const
{
    StringBuilder builder(StringBuilder::Mode::UTF16, length_in_code_units());

    for (size_t i = 0; i < length_in_code_units(); ++i)
        builder.append_code_unit(AK::to_ascii_lowercase(code_unit_at(i)));

    return builder.to_utf16_string();
}

Utf16String Utf16View::to_ascii_uppercase() const
{
    StringBuilder builder(StringBuilder::Mode::UTF16, length_in_code_units());

    for (size_t i = 0; i < length_in_code_units(); ++i)
        builder.append_code_unit(AK::to_ascii_uppercase(code_unit_at(i)));

    return builder.to_utf16_string();
}

Utf16String Utf16View::to_ascii_titlecase() const
{
    StringBuilder builder(StringBuilder::Mode::UTF16, length_in_code_units());
    bool next_is_upper = true;

    for (size_t i = 0; i < length_in_code_units(); ++i) {
        auto code_unit = code_unit_at(i);

        if (next_is_upper)
            builder.append_code_unit(AK::to_ascii_uppercase(code_unit));
        else
            builder.append_code_unit(AK::to_ascii_lowercase(code_unit));

        next_is_upper = code_unit == u' ';
    }

    return builder.to_utf16_string();
}

Utf16String Utf16View::replace(Utf16View const& needle, Utf16View const& replacement, ReplaceMode replace_mode) const
{
    if (is_empty())
        return {};

    StringBuilder builder(StringBuilder::Mode::UTF16, length_in_code_units());
    auto remaining = *this;

    do {
        auto index = remaining.find_code_unit_offset(needle);
        if (!index.has_value())
            break;

        builder.append(remaining.substring_view(0, *index));
        builder.append(replacement);

        remaining = remaining.substring_view(*index + needle.length_in_code_units());
        index = remaining.find_code_unit_offset(needle);
    } while (replace_mode == ReplaceMode::All && !remaining.is_empty());

    builder.append(remaining);
    return builder.to_utf16_string();
}

Utf16String Utf16View::escape_html_entities() const
{
    StringBuilder builder(StringBuilder::Mode::UTF16, length_in_code_units());

    for (auto code_point : *this) {
        if (code_point == '<')
            builder.append(u"&lt;"sv);
        else if (code_point == '>')
            builder.append(u"&gt;"sv);
        else if (code_point == '&')
            builder.append(u"&amp;"sv);
        else if (code_point == '"')
            builder.append(u"&quot;"sv);
        else
            builder.append_code_point(code_point);
    }

    return builder.to_utf16_string();
}

bool Utf16View::is_ascii() const
{
    if (has_ascii_storage())
        return true;

    // FIXME: Petition simdutf to implement an ASCII validator for UTF-16.
    return all_of(utf16_span(), AK::is_ascii);
}

bool Utf16View::validate(size_t& valid_code_units, AllowLonelySurrogates allow_lonely_surrogates) const
{
    if (has_ascii_storage()) {
        valid_code_units = length_in_code_units();
        return true;
    }

    auto view = *this;
    valid_code_units = 0;

    while (!view.is_empty()) {
        auto result = simdutf::validate_utf16_with_errors(view.m_string.utf16, view.length_in_code_units());
        valid_code_units += result.count;

        if (result.error == simdutf::SUCCESS)
            return true;
        if (allow_lonely_surrogates == AllowLonelySurrogates::No || result.error != simdutf::SURROGATE)
            return false;

        view = view.substring_view(result.count + 1);
        ++valid_code_units;
    }

    return true;
}

size_t Utf16View::code_unit_offset_of(size_t code_point_offset) const
{
    VERIFY(code_point_offset <= length_in_code_points());

    if (length_in_code_points() == length_in_code_units()) // Fast path: all code points are one code unit.
        return code_point_offset;

    size_t code_unit_offset = 0;

    for (auto it = begin(); it != end(); ++it) {
        if (code_point_offset == 0)
            return code_unit_offset;

        code_unit_offset += it.length_in_code_units();
        --code_point_offset;
    }

    return code_unit_offset;
}

size_t Utf16View::code_point_offset_of(size_t code_unit_offset) const
{
    VERIFY(code_unit_offset <= length_in_code_units());

    if (length_in_code_points() == length_in_code_units()) // Fast path: all code points are one code unit.
        return code_unit_offset;

    size_t code_point_offset = 0;

    for (auto it = begin(); it != end();) {
        // We know the view is using UTF-16 storage because ASCII storage would have returned early above.
        if ((++it).m_iterator.utf16 > m_string.utf16 + code_unit_offset)
            break;
        ++code_point_offset;
    }

    return code_point_offset;
}

Utf16View Utf16View::unicode_substring_view(size_t code_point_offset, size_t code_point_length) const
{
    if (code_point_length == 0)
        return {};

    if (length_in_code_points() == length_in_code_units()) // Fast path: all code points are one code unit.
        return substring_view(code_point_offset, code_point_length);

    auto code_unit_offset_of = [&](Utf16CodePointIterator const& it) {
        if (has_ascii_storage())
            return it.m_iterator.ascii - m_string.ascii;
        return it.m_iterator.utf16 - m_string.utf16;
    };

    size_t code_point_index = 0;
    size_t code_unit_offset = 0;

    for (auto it = begin(); it != end(); ++it) {
        if (code_point_index == code_point_offset)
            code_unit_offset = code_unit_offset_of(it);

        if (code_point_index == (code_point_offset + code_point_length - 1)) {
            size_t code_unit_length = code_unit_offset_of(++it) - code_unit_offset;
            return substring_view(code_unit_offset, code_unit_length);
        }

        ++code_point_index;
    }

    VERIFY_NOT_REACHED();
}

size_t Utf16View::calculate_length_in_code_points() const
{
    ASSERT(!has_ascii_storage());

    // simdutf's code point length method assumes valid UTF-16, whereas we allow lonely surrogates.
    if (validate(AllowLonelySurrogates::No)) [[likely]]
        return simdutf::count_utf16(m_string.utf16, length_in_code_units());

    size_t code_points = 0;
    for ([[maybe_unused]] auto code_point : *this)
        ++code_points;
    return code_points;
}

}
