/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/CharacterTypes.h>
#include <AK/Concepts.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>
#include <AK/Utf8View.h>

#include <simdutf.h>

namespace AK {

static constexpr u16 high_surrogate_min = 0xd800;
static constexpr u16 high_surrogate_max = 0xdbff;
static constexpr u16 low_surrogate_min = 0xdc00;
static constexpr u16 low_surrogate_max = 0xdfff;
static constexpr u32 replacement_code_point = 0xfffd;
static constexpr u32 first_supplementary_plane_code_point = 0x10000;

template<OneOf<Utf8View, Utf32View> UtfViewType>
static ErrorOr<Utf16Data> to_utf16_slow(UtfViewType const& view)
{
    Utf16Data utf16_data;
    TRY(utf16_data.try_ensure_capacity(view.length()));

    for (auto code_point : view)
        TRY(code_point_to_utf16(utf16_data, code_point));

    return utf16_data;
}

ErrorOr<Utf16Data> utf8_to_utf16(StringView utf8_view)
{
    return utf8_to_utf16(Utf8View { utf8_view });
}

ErrorOr<Utf16Data> utf8_to_utf16(Utf8View const& utf8_view)
{
    // All callers want to allow lonely surrogates, which simdutf does not permit.
    if (!utf8_view.validate(Utf8View::AllowSurrogates::No)) [[unlikely]]
        return to_utf16_slow(utf8_view);

    Utf16Data utf16_data;

    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf8(
        reinterpret_cast<char const*>(utf8_view.bytes()),
        utf8_view.byte_length())));

    [[maybe_unused]] auto result = simdutf::convert_utf8_to_utf16(
        reinterpret_cast<char const*>(utf8_view.bytes()),
        utf8_view.byte_length(),
        reinterpret_cast<char16_t*>(utf16_data.data()));
    ASSERT(result == utf16_data.size());

    return utf16_data;
}

ErrorOr<Utf16Data> utf32_to_utf16(Utf32View const& utf32_view)
{
    Utf16Data utf16_data;

    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf32(
        reinterpret_cast<char32_t const*>(utf32_view.code_points()),
        utf32_view.length())));

    [[maybe_unused]] auto result = simdutf::convert_utf32_to_utf16(
        reinterpret_cast<char32_t const*>(utf32_view.code_points()),
        utf32_view.length(),
        reinterpret_cast<char16_t*>(utf16_data.data()));
    ASSERT(result == utf16_data.size());

    return utf16_data;
}

ErrorOr<void> code_point_to_utf16(Utf16Data& string, u32 code_point)
{
    VERIFY(is_unicode(code_point));

    if (code_point < first_supplementary_plane_code_point) {
        TRY(string.try_append(static_cast<u16>(code_point)));
    } else {
        code_point -= first_supplementary_plane_code_point;
        TRY(string.try_append(static_cast<u16>(high_surrogate_min | (code_point >> 10))));
        TRY(string.try_append(static_cast<u16>(low_surrogate_min | (code_point & 0x3ff))));
    }

    return {};
}

bool Utf16View::is_high_surrogate(u16 code_unit)
{
    return (code_unit >= high_surrogate_min) && (code_unit <= high_surrogate_max);
}

bool Utf16View::is_low_surrogate(u16 code_unit)
{
    return (code_unit >= low_surrogate_min) && (code_unit <= low_surrogate_max);
}

u32 Utf16View::decode_surrogate_pair(u16 high_surrogate, u16 low_surrogate)
{
    VERIFY(is_high_surrogate(high_surrogate));
    VERIFY(is_low_surrogate(low_surrogate));

    return ((high_surrogate - high_surrogate_min) << 10) + (low_surrogate - low_surrogate_min) + first_supplementary_plane_code_point;
}

ErrorOr<ByteString> Utf16View::to_byte_string(AllowInvalidCodeUnits allow_invalid_code_units) const
{
    return TRY(to_utf8(allow_invalid_code_units)).to_byte_string();
}

ErrorOr<String> Utf16View::to_utf8(AllowInvalidCodeUnits allow_invalid_code_units) const
{
    if (allow_invalid_code_units == AllowInvalidCodeUnits::No)
        return String::from_utf16(*this);

    StringBuilder builder;

    for (auto const* ptr = begin_ptr(); ptr < end_ptr(); ++ptr) {
        if (is_high_surrogate(*ptr)) {
            auto const* next = ptr + 1;

            if ((next < end_ptr()) && is_low_surrogate(*next)) {
                auto code_point = decode_surrogate_pair(*ptr, *next);
                TRY(builder.try_append_code_point(code_point));
                ++ptr;
                continue;
            }
        }

        TRY(builder.try_append_code_point(static_cast<u32>(*ptr)));
    }

    return builder.to_string_without_validation();
}

size_t Utf16View::length_in_code_points() const
{
    if (!m_length_in_code_points.has_value())
        m_length_in_code_points = calculate_length_in_code_points();
    return *m_length_in_code_points;
}

u16 Utf16View::code_unit_at(size_t index) const
{
    VERIFY(index < length_in_code_units());
    return m_code_units[index];
}

u32 Utf16View::code_point_at(size_t index) const
{
    VERIFY(index < length_in_code_units());

    u32 code_point = code_unit_at(index);
    if (!is_high_surrogate(code_point) && !is_low_surrogate(code_point))
        return code_point;
    if (is_low_surrogate(code_point) || (index + 1 == length_in_code_units()))
        return code_point;

    auto second = code_unit_at(index + 1);
    if (!is_low_surrogate(second))
        return code_point;

    return decode_surrogate_pair(code_point, second);
}

size_t Utf16View::code_point_offset_of(size_t code_unit_offset) const
{
    size_t code_point_offset = 0;

    for (auto it = begin(); it != end(); ++it) {
        if (code_unit_offset == 0)
            return code_point_offset;

        code_unit_offset -= it.length_in_code_units();
        ++code_point_offset;
    }

    return code_point_offset;
}

size_t Utf16View::code_unit_offset_of(size_t code_point_offset) const
{
    size_t code_unit_offset = 0;

    for (auto it = begin(); it != end(); ++it) {
        if (code_point_offset == 0)
            return code_unit_offset;

        code_unit_offset += it.length_in_code_units();
        --code_point_offset;
    }

    return code_unit_offset;
}

size_t Utf16View::code_unit_offset_of(Utf16CodePointIterator const& it) const
{
    VERIFY(it.m_ptr >= begin_ptr());
    VERIFY(it.m_ptr <= end_ptr());

    return it.m_ptr - begin_ptr();
}

Utf16View Utf16View::substring_view(size_t code_unit_offset, size_t code_unit_length) const
{
    VERIFY(!Checked<size_t>::addition_would_overflow(code_unit_offset, code_unit_length));
    VERIFY(code_unit_offset + code_unit_length <= length_in_code_units());

    return Utf16View { m_code_units.slice(code_unit_offset, code_unit_length) };
}

Utf16View Utf16View::unicode_substring_view(size_t code_point_offset, size_t code_point_length) const
{
    if (code_point_length == 0)
        return {};

    auto code_unit_offset_of = [&](Utf16CodePointIterator const& it) { return it.m_ptr - begin_ptr(); };
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

bool Utf16View::starts_with(Utf16View const& needle) const
{
    if (needle.is_empty())
        return true;
    if (is_empty())
        return false;
    if (needle.length_in_code_units() > length_in_code_units())
        return false;
    if (begin_ptr() == needle.begin_ptr())
        return true;

    for (auto this_it = begin(), needle_it = needle.begin(); needle_it != needle.end(); ++needle_it, ++this_it) {
        if (*this_it != *needle_it)
            return false;
    }

    return true;
}

bool Utf16View::validate() const
{
    return simdutf::validate_utf16(reinterpret_cast<char16_t const*>(m_code_units.data()), m_code_units.size());
}

bool Utf16View::validate(size_t& valid_code_units) const
{
    auto result = simdutf::validate_utf16_with_errors(reinterpret_cast<char16_t const*>(m_code_units.data()), m_code_units.size());
    valid_code_units = result.count;

    return result.error == simdutf::SUCCESS;
}

size_t Utf16View::calculate_length_in_code_points() const
{
    // FIXME: simdutf's code point length method assumes valid UTF-16, whereas Utf16View uses U+FFFD as a replacement
    //        for invalid code points. If we change Utf16View to only accept valid encodings as an invariant, we can
    //        remove this branch.
    if (validate()) [[likely]]
        return simdutf::count_utf16(reinterpret_cast<char16_t const*>(m_code_units.data()), m_code_units.size());

    size_t code_points = 0;
    for ([[maybe_unused]] auto code_point : *this)
        ++code_points;
    return code_points;
}

bool Utf16View::equals_ignoring_case(Utf16View const& other) const
{
    if (length_in_code_units() == 0)
        return other.length_in_code_units() == 0;
    if (length_in_code_units() != other.length_in_code_units())
        return false;

    for (size_t i = 0; i < length_in_code_units(); ++i) {
        // FIXME: Handle non-ASCII case insensitive comparisons.
        if (to_ascii_lowercase(m_code_units[i]) != to_ascii_lowercase(other.m_code_units[i]))
            return false;
    }

    return true;
}

Utf16CodePointIterator& Utf16CodePointIterator::operator++()
{
    size_t code_units = length_in_code_units();

    if (code_units > m_remaining_code_units) {
        // If there aren't enough code units remaining, skip to the end.
        m_ptr += m_remaining_code_units;
        m_remaining_code_units = 0;
    } else {
        m_ptr += code_units;
        m_remaining_code_units -= code_units;
    }

    return *this;
}

u32 Utf16CodePointIterator::operator*() const
{
    VERIFY(m_remaining_code_units > 0);

    // rfc2781, 2.2 Decoding UTF-16
    // 1) If W1 < 0xD800 or W1 > 0xDFFF, the character value U is the value
    //    of W1. Terminate.
    // 2) Determine if W1 is between 0xD800 and 0xDBFF. If not, the sequence
    //    is in error and no valid character can be obtained using W1.
    //    Terminate.
    // 3) If there is no W2 (that is, the sequence ends with W1), or if W2
    //    is not between 0xDC00 and 0xDFFF, the sequence is in error.
    //    Terminate.
    // 4) Construct a 20-bit unsigned integer U', taking the 10 low-order
    //    bits of W1 as its 10 high-order bits and the 10 low-order bits of
    //    W2 as its 10 low-order bits.
    // 5) Add 0x10000 to U' to obtain the character value U. Terminate.

    if (Utf16View::is_high_surrogate(*m_ptr)) {
        if ((m_remaining_code_units > 1) && Utf16View::is_low_surrogate(*(m_ptr + 1)))
            return Utf16View::decode_surrogate_pair(*m_ptr, *(m_ptr + 1));
        return replacement_code_point;
    } else if (Utf16View::is_low_surrogate(*m_ptr)) {
        return replacement_code_point;
    }

    return static_cast<u32>(*m_ptr);
}

size_t Utf16CodePointIterator::length_in_code_units() const
{
    VERIFY(m_remaining_code_units > 0);

    if (Utf16View::is_high_surrogate(*m_ptr)) {
        if ((m_remaining_code_units > 1) && Utf16View::is_low_surrogate(*(m_ptr + 1)))
            return 2;
    }

    // If this return is reached, either the encoded code point is a valid single code unit, or that
    // code point is invalid (e.g. began with a low surrogate, or a low surrogate did not follow a
    // high surrogate). In the latter case, a single replacement code unit will be used.
    return 1;
}

}
