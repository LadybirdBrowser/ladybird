/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
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

static constexpr u16 high_surrogate_min = 0xd800;
static constexpr u16 high_surrogate_max = 0xdbff;
static constexpr u16 low_surrogate_min = 0xdc00;
static constexpr u16 low_surrogate_max = 0xdfff;
static constexpr u32 replacement_code_point = 0xfffd;
static constexpr u32 first_supplementary_plane_code_point = 0x10000;

static constexpr u16 host_code_unit(u16 code_unit, Endianness endianness)
{
    switch (endianness) {
    case Endianness::Host:
        return code_unit;
    case Endianness::Big:
        return convert_between_host_and_big_endian(code_unit);
    case Endianness::Little:
        return convert_between_host_and_little_endian(code_unit);
    }
    VERIFY_NOT_REACHED();
}

template<OneOf<Utf8View, Utf32View> UtfViewType>
static ErrorOr<Utf16ConversionResult> to_utf16_slow(UtfViewType const& view, Endianness endianness)
{
    Utf16Data utf16_data;
    TRY(utf16_data.try_ensure_capacity(view.length()));

    size_t code_point_count = 0;
    for (auto code_point : view) {
        TRY(code_point_to_utf16(utf16_data, code_point, endianness));
        code_point_count++;
    }

    return Utf16ConversionResult { move(utf16_data), code_point_count };
}

ErrorOr<Utf16ConversionResult> utf8_to_utf16(StringView utf8_view, Endianness endianness)
{
    return utf8_to_utf16(Utf8View { utf8_view }, endianness);
}

ErrorOr<Utf16ConversionResult> utf8_to_utf16(Utf8View const& utf8_view, Endianness endianness)
{
    // All callers want to allow lonely surrogates, which simdutf does not permit.
    if (!utf8_view.validate(Utf8View::AllowSurrogates::No)) [[unlikely]]
        return to_utf16_slow(utf8_view, endianness);
    if (utf8_view.is_empty())
        return Utf16ConversionResult { Utf16Data {}, 0 };

    auto const* data = reinterpret_cast<char const*>(utf8_view.bytes());
    auto length = utf8_view.byte_length();

    Utf16Data utf16_data;
    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf8(data, length)));
    // FIXME: simdutf _could_ be telling us about this, but it doesn't -- so we have to compute it again.
    auto code_point_length = simdutf::count_utf8(data, length);

    [[maybe_unused]] auto result = [&]() {
        switch (endianness) {
        case Endianness::Host:
            return simdutf::convert_utf8_to_utf16(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        case Endianness::Big:
            return simdutf::convert_utf8_to_utf16be(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        case Endianness::Little:
            return simdutf::convert_utf8_to_utf16le(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        }
        VERIFY_NOT_REACHED();
    }();
    ASSERT(result == utf16_data.size());

    return Utf16ConversionResult { utf16_data, code_point_length };
}

ErrorOr<Utf16ConversionResult> utf32_to_utf16(Utf32View const& utf32_view, Endianness endianness)
{
    if (utf32_view.is_empty())
        return Utf16ConversionResult { Utf16Data {}, 0 };

    auto const* data = reinterpret_cast<char32_t const*>(utf32_view.code_points());
    auto length = utf32_view.length();

    Utf16Data utf16_data;
    TRY(utf16_data.try_resize(simdutf::utf16_length_from_utf32(data, length)));

    [[maybe_unused]] auto result = [&]() {
        switch (endianness) {
        case Endianness::Host:
            return simdutf::convert_utf32_to_utf16(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        case Endianness::Big:
            return simdutf::convert_utf32_to_utf16be(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        case Endianness::Little:
            return simdutf::convert_utf32_to_utf16le(data, length, reinterpret_cast<char16_t*>(utf16_data.data()));
        }
        VERIFY_NOT_REACHED();
    }();
    ASSERT(result == utf16_data.size());

    return Utf16ConversionResult { utf16_data, length };
}

ErrorOr<void> code_point_to_utf16(Utf16Data& string, u32 code_point, Endianness endianness)
{
    VERIFY(is_unicode(code_point));

    if (code_point < first_supplementary_plane_code_point) {
        TRY(string.try_append(host_code_unit(static_cast<u16>(code_point), endianness)));
    } else {
        code_point -= first_supplementary_plane_code_point;

        auto code_unit = static_cast<u16>(high_surrogate_min | (code_point >> 10));
        TRY(string.try_append(host_code_unit(code_unit, endianness)));

        code_unit = static_cast<u16>(low_surrogate_min | (code_point & 0x3ff));
        TRY(string.try_append(host_code_unit(code_unit, endianness)));
    }

    return {};
}

size_t utf16_code_unit_length_from_utf8(StringView string)
{
    return simdutf::utf16_length_from_utf8(string.characters_without_null_termination(), string.length());
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
    builder.append(*this);
    return builder.to_string();
}

size_t Utf16View::length_in_code_points() const
{
    if (m_length_in_code_points == NumericLimits<size_t>::max())
        m_length_in_code_points = calculate_length_in_code_points();
    return m_length_in_code_points;
}

u16 Utf16View::code_unit_at(size_t index) const
{
    VERIFY(index < length_in_code_units());
    return host_code_unit(m_code_units[index], Endianness::Host);
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
    if (m_length_in_code_points == m_code_units.size()) // Fast path: all code points are one code unit.
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

size_t Utf16View::code_unit_offset_of(size_t code_point_offset) const
{
    if (m_length_in_code_points == m_code_units.size()) // Fast path: all code points are one code unit.
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

    if (m_length_in_code_points == m_code_units.size()) // Fast path: all code points are one code unit.
        return substring_view(code_point_offset, code_point_length);

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

// https://infra.spec.whatwg.org/#code-unit-less-than
bool Utf16View::is_code_unit_less_than(Utf16View const& other) const
{
    auto a = m_code_units;
    auto b = other.m_code_units;

    auto common_length = min(a.size(), b.size());

    for (size_t position = 0; position < common_length; ++position) {
        if (a[position] != b[position])
            return a[position] < b[position];
    }

    return a.size() < b.size();
}

bool Utf16View::validate() const
{
    return simdutf::validate_utf16(char_data(), length_in_code_units());
}

bool Utf16View::validate(size_t& valid_code_units) const
{
    auto result = simdutf::validate_utf16_with_errors(char_data(), length_in_code_units());
    valid_code_units = result.count;
    return result.error == simdutf::SUCCESS;
}

size_t Utf16View::calculate_length_in_code_points() const
{
    // FIXME: simdutf's code point length method assumes valid UTF-16, whereas Utf16View uses U+FFFD as a replacement
    //        for invalid code points. If we change Utf16View to only accept valid encodings as an invariant, we can
    //        remove this branch.
    if (validate()) [[likely]]
        return simdutf::count_utf16(char_data(), length_in_code_units());

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

    auto code_unit = host_code_unit(*m_ptr, Endianness::Host);

    if (Utf16View::is_high_surrogate(code_unit)) {
        if (m_remaining_code_units > 1) {
            auto next_code_unit = host_code_unit(*(m_ptr + 1), Endianness::Host);

            if (Utf16View::is_low_surrogate(next_code_unit))
                return Utf16View::decode_surrogate_pair(code_unit, next_code_unit);
        }

        return replacement_code_point;
    }

    if (Utf16View::is_low_surrogate(code_unit))
        return replacement_code_point;

    return static_cast<u32>(code_unit);
}

size_t Utf16CodePointIterator::length_in_code_units() const
{
    return *(*this) < first_supplementary_plane_code_point ? 1 : 2;
}

bool validate_utf16_le(ReadonlyBytes bytes)
{
    return simdutf::validate_utf16le(reinterpret_cast<char16_t const*>(bytes.data()), bytes.size() / 2);
}

bool validate_utf16_be(ReadonlyBytes bytes)
{
    return simdutf::validate_utf16be(reinterpret_cast<char16_t const*>(bytes.data()), bytes.size() / 2);
}

}
