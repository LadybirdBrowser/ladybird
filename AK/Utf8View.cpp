/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Utf8View.h>

#include <simdutf.h>

namespace AK {

Utf8CodePointIterator Utf8View::iterator_at_byte_offset(size_t byte_offset) const
{
    size_t current_offset = 0;
    for (auto iterator = begin(); !iterator.done(); ++iterator) {
        if (current_offset >= byte_offset)
            return iterator;
        current_offset += iterator.underlying_code_point_length_in_bytes();
    }
    return end();
}

Utf8CodePointIterator Utf8View::iterator_at_byte_offset_without_validation(size_t byte_offset) const
{
    return Utf8CodePointIterator { reinterpret_cast<u8 const*>(m_string.characters_without_null_termination()) + byte_offset, m_string.length() - byte_offset };
}

size_t Utf8View::byte_offset_of(Utf8CodePointIterator const& it) const
{
    VERIFY(it.m_ptr >= begin_ptr());
    VERIFY(it.m_ptr <= end_ptr());

    return it.m_ptr - begin_ptr();
}

size_t Utf8View::byte_offset_of(size_t code_point_offset) const
{
    size_t byte_offset = 0;

    for (auto it = begin(); !it.done(); ++it) {
        if (code_point_offset == 0)
            return byte_offset;

        byte_offset += it.underlying_code_point_length_in_bytes();
        --code_point_offset;
    }

    return byte_offset;
}

Utf8View Utf8View::unicode_substring_view(size_t code_point_offset, size_t code_point_length) const
{
    if (code_point_length == 0)
        return {};

    size_t code_point_index = 0, offset_in_bytes = 0;
    for (auto iterator = begin(); !iterator.done(); ++iterator) {
        if (code_point_index == code_point_offset)
            offset_in_bytes = byte_offset_of(iterator);
        if (code_point_index == code_point_offset + code_point_length - 1) {
            size_t length_in_bytes = byte_offset_of(++iterator) - offset_in_bytes;
            return substring_view(offset_in_bytes, length_in_bytes);
        }
        ++code_point_index;
    }

    VERIFY_NOT_REACHED();
}

size_t Utf8View::calculate_length() const
{
    // FIXME: simdutf's code point length method assumes valid UTF-8, whereas Utf8View uses U+FFFD as a replacement
    //        for invalid code points. If we change Utf8View to only accept valid encodings as an invariant, we can
    //        remove this branch.
    if (validate()) [[likely]]
        return simdutf::count_utf8(m_string.characters_without_null_termination(), m_string.length());

    size_t length = 0;

    for (size_t i = 0; i < m_string.length(); ++length) {
        auto [byte_length, code_point, is_valid] = decode_leading_byte(static_cast<u8>(m_string[i]));

        // Similar to Utf8CodePointIterator::operator++, if the byte is invalid, try the next byte.
        i += is_valid ? byte_length : 1;
    }

    return length;
}

bool Utf8View::starts_with(Utf8View const& start) const
{
    if (start.is_empty())
        return true;
    if (is_empty())
        return false;
    if (start.length() > length())
        return false;
    if (begin_ptr() == start.begin_ptr())
        return true;

    for (auto k = begin(), l = start.begin(); l != start.end(); ++k, ++l) {
        if (*k != *l)
            return false;
    }
    return true;
}

bool Utf8View::contains(u32 needle) const
{
    for (u32 code_point : *this) {
        if (code_point == needle)
            return true;
    }
    return false;
}

Utf8View Utf8View::trim(Utf8View const& characters, TrimMode mode) const
{
    size_t substring_start = 0;
    size_t substring_length = byte_length();

    if (mode == TrimMode::Left || mode == TrimMode::Both) {
        for (auto code_point = begin(); code_point != end(); ++code_point) {
            if (substring_length == 0)
                return {};
            if (!characters.contains(*code_point))
                break;
            substring_start += code_point.underlying_code_point_length_in_bytes();
            substring_length -= code_point.underlying_code_point_length_in_bytes();
        }
    }

    if (mode == TrimMode::Right || mode == TrimMode::Both) {
        size_t seen_whitespace_length = 0;
        for (auto code_point = begin(); code_point != end(); ++code_point) {
            if (characters.contains(*code_point))
                seen_whitespace_length += code_point.underlying_code_point_length_in_bytes();
            else
                seen_whitespace_length = 0;
        }
        if (seen_whitespace_length >= substring_length)
            return {};
        substring_length -= seen_whitespace_length;
    }

    return substring_view(substring_start, substring_length);
}

bool Utf8View::validate(size_t& valid_bytes, AllowSurrogates allow_surrogates) const
{
    auto result = simdutf::validate_utf8_with_errors(m_string.characters_without_null_termination(), m_string.length());
    valid_bytes = result.count;

    if (result.error == simdutf::SURROGATE && allow_surrogates == AllowSurrogates::Yes) {
        valid_bytes += 3; // All surrogates have a UTF-8 byte length of 3.

        size_t substring_valid_bytes = 0;
        auto is_valid = substring_view(valid_bytes).validate(substring_valid_bytes, allow_surrogates);

        valid_bytes += substring_valid_bytes;
        return is_valid;
    }

    return result.error == simdutf::SUCCESS;
}

Utf8CodePointIterator& Utf8CodePointIterator::operator++()
{
    VERIFY(m_length > 0);

    // OPTIMIZATION: Fast path for ASCII characters.
    if (*m_ptr <= 0x7F) {
        m_ptr += 1;
        m_length -= 1;
        return *this;
    }

    size_t code_point_length_in_bytes = underlying_code_point_length_in_bytes();
    if (code_point_length_in_bytes > m_length) {
        // We don't have enough data for the next code point. Skip one character and try again.
        // The rest of the code will output replacement characters as needed for any eventual extension bytes we might encounter afterwards.
        dbgln_if(UTF8_DEBUG, "Expected code point size {} is too big for the remaining length {}. Moving forward one byte.", code_point_length_in_bytes, m_length);
        m_ptr += 1;
        m_length -= 1;
        return *this;
    }

    m_ptr += code_point_length_in_bytes;
    m_length -= code_point_length_in_bytes;
    return *this;
}

size_t Utf8CodePointIterator::underlying_code_point_length_in_bytes() const
{
    VERIFY(m_length > 0);
    auto [code_point_length_in_bytes, value, first_byte_makes_sense] = Utf8View::decode_leading_byte(*m_ptr);

    // If any of these tests fail, we will output a replacement character for this byte and treat it as a code point of size 1.
    if (!first_byte_makes_sense)
        return 1;

    if (code_point_length_in_bytes > m_length)
        return 1;

    for (size_t offset = 1; offset < code_point_length_in_bytes; offset++) {
        if (m_ptr[offset] >> 6 != 2)
            return 1;
    }

    return code_point_length_in_bytes;
}

ReadonlyBytes Utf8CodePointIterator::underlying_code_point_bytes() const
{
    return { m_ptr, underlying_code_point_length_in_bytes() };
}

u32 Utf8CodePointIterator::operator*() const
{
    VERIFY(m_length > 0);

    // OPTIMIZATION: Fast path for ASCII characters.
    if (*m_ptr <= 0x7F)
        return *m_ptr;

    auto [code_point_length_in_bytes, code_point_value_so_far, first_byte_makes_sense] = Utf8View::decode_leading_byte(*m_ptr);

    if (!first_byte_makes_sense) {
        // The first byte of the code point doesn't make sense: output a replacement character
        dbgln_if(UTF8_DEBUG, "First byte doesn't make sense: {:#02x}.", m_ptr[0]);
        return 0xFFFD;
    }

    if (code_point_length_in_bytes > m_length) {
        // There is not enough data left for the full code point: output a replacement character
        dbgln_if(UTF8_DEBUG, "Not enough bytes (need {}, have {}), first byte is: {:#02x}.", code_point_length_in_bytes, m_length, m_ptr[0]);
        return 0xFFFD;
    }

    for (size_t offset = 1; offset < code_point_length_in_bytes; offset++) {
        if (m_ptr[offset] >> 6 != 2) {
            // One of the extension bytes of the code point doesn't make sense: output a replacement character
            dbgln_if(UTF8_DEBUG, "Extension byte {:#02x} in {} position after first byte {:#02x} doesn't make sense.", m_ptr[offset], offset, m_ptr[0]);
            return 0xFFFD;
        }

        code_point_value_so_far <<= 6;
        code_point_value_so_far |= m_ptr[offset] & 63;
    }

    if (code_point_value_so_far > 0x10FFFF) {
        dbgln_if(UTF8_DEBUG, "Multi-byte sequence is otherwise valid, but code point {:#x} is not permissible.", code_point_value_so_far);
        return 0xFFFD;
    }
    return code_point_value_so_far;
}

Optional<u32> Utf8CodePointIterator::peek(size_t offset) const
{
    if (offset == 0) {
        if (this->done())
            return {};
        return this->operator*();
    }

    auto new_iterator = *this;
    for (size_t index = 0; index < offset; ++index) {
        ++new_iterator;
        if (new_iterator.done())
            return {};
    }
    return *new_iterator;
}

ErrorOr<void> Formatter<Utf8View>::format(FormatBuilder& builder, Utf8View const& string)
{
    return Formatter<StringView>::format(builder, string.as_string());
}

}
