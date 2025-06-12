/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Concepts.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>

#include <simdutf.h>

namespace AK {

template<OneOf<Utf8View, Utf32View> UtfViewType>
static ErrorOr<Utf16ConversionResult> to_utf16_slow(UtfViewType const& view)
{
    Utf16Data utf16_data;
    TRY(utf16_data.try_ensure_capacity(view.length()));

    size_t code_point_count = 0;
    for (auto code_point : view) {
        TRY(UnicodeUtils::try_code_point_to_utf16(code_point, [&](auto code_unit) -> ErrorOr<void> {
            TRY(utf16_data.try_append(code_unit));
            return {};
        }));

        code_point_count++;
    }

    return Utf16ConversionResult { move(utf16_data), code_point_count };
}

ErrorOr<Utf16ConversionResult> utf8_to_utf16(StringView utf8_view)
{
    return utf8_to_utf16(Utf8View { utf8_view });
}

ErrorOr<Utf16ConversionResult> utf8_to_utf16(Utf8View const& utf8_view)
{
    if (utf8_view.is_empty())
        return Utf16ConversionResult { Utf16Data {}, 0 };

    // All callers want to allow lonely surrogates, which simdutf does not permit.
    if (!utf8_view.validate(AllowLonelySurrogates::No)) [[unlikely]]
        return to_utf16_slow(utf8_view);

    auto const* data = reinterpret_cast<char const*>(utf8_view.bytes());
    auto length = utf8_view.byte_length();

    Utf16Data utf16_data;
    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf8(data, length)));
    // FIXME: simdutf _could_ be telling us about this, but it doesn't -- so we have to compute it again.
    auto code_point_length = simdutf::count_utf8(data, length);

    [[maybe_unused]] auto result = simdutf::convert_utf8_to_utf16(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
    ASSERT(result == utf16_data.size());

    return Utf16ConversionResult { utf16_data, code_point_length };
}

ErrorOr<Utf16ConversionResult> utf32_to_utf16(Utf32View const& utf32_view)
{
    if (utf32_view.is_empty())
        return Utf16ConversionResult { Utf16Data {}, 0 };

    auto const* data = reinterpret_cast<char32_t const*>(utf32_view.code_points());
    auto length = utf32_view.length();

    Utf16Data utf16_data;
    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf32(data, length)));

    [[maybe_unused]] auto result = simdutf::convert_utf32_to_utf16(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
    ASSERT(result == utf16_data.size());

    return Utf16ConversionResult { utf16_data, length };
}

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
    if (length_in_code_points() == length_in_code_units()) // Fast path: all code points are one code unit.
        return code_unit_offset;

    size_t code_point_offset = 0;

    for (auto it = begin(); it != end(); ++it) {
        if (code_unit_offset == 0)
            return code_point_offset;

        code_unit_offset -= it.length_in_code_units();
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
