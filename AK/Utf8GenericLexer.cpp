/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/CharacterTypes.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8GenericLexer.h>

namespace AK {

// FIXME: All static_cast<u32>s should be changed to explicit \U char32_t codepoints

Utf8View Utf8GenericLexer::consume(size_t code_point_count)
{
    auto start_offset = tell();
    size_t consumed = 0;

    while (!is_eof() && consumed < code_point_count) {
        ++m_iterator;
        ++consumed;
    }

    auto end_offset = tell();
    return m_input.substring_view(start_offset, end_offset - start_offset);
}

// Consume the rest of the input
Utf8View Utf8GenericLexer::consume_all()
{
    auto start_offset = tell();
    while (!is_eof()) {
        ++m_iterator;
    }
    auto end_offset = tell();
    return m_input.substring_view(start_offset, end_offset - start_offset);
}

// Consume until a new line is found
Utf8View Utf8GenericLexer::consume_line()
{
    auto start_offset = tell();
    while (!is_eof() && !is_newline_unicode(peek())) {
        ++m_iterator;
    }
    auto end_offset = tell();

    // Consume line ending
    if (!is_eof()) {
        auto line_ending = peek();
        ++m_iterator;

        // Handle CRLF
        if (line_ending == static_cast<u32>('\r') && !is_eof() && peek() == static_cast<u32>('\n')) {
            ++m_iterator;
        }
    }

    return m_input.substring_view(start_offset, end_offset - start_offset);
}

// Consume and return code points until `stop` is encountered
Utf8View Utf8GenericLexer::consume_until(u32 stop)
{
    auto start_offset = tell();
    while (!is_eof() && peek() != stop) {
        ++m_iterator;
    }
    auto end_offset = tell();
    return m_input.substring_view(start_offset, end_offset - start_offset);
}

// Consume and return code points until the string `stop` is found
Utf8View Utf8GenericLexer::consume_until(Utf8View stop)
{
    auto start_offset = tell();
    while (!is_eof() && !next_is(stop)) {
        ++m_iterator;
    }
    auto end_offset = tell();
    return m_input.substring_view(start_offset, end_offset - start_offset);
}

Utf8View Utf8GenericLexer::consume_quoted_string(u32 escape_char)
{
    constexpr u32 single_quote = static_cast<u32>('\'');
    constexpr u32 double_quote = static_cast<u32>('"');

    if (!next_is(single_quote) && !next_is(double_quote)) {
        return {};
    }

    u32 quote_char = consume();
    auto start_offset = tell();

    while (!is_eof()) {
        if (escape_char != 0 && next_is(escape_char)) {
            ++m_iterator; // Skip escape char
            if (!is_eof()) {
                ++m_iterator; // Skip escaped char
            }
        } else if (next_is(quote_char)) {
            break;
        } else {
            ++m_iterator;
        }
    }

    auto end_offset = tell();

    if (is_eof() || peek() != quote_char) {
        // Restore the iterator in case the string is unterminated
        m_iterator = m_input.iterator_at_byte_offset_without_validation(start_offset - m_input.byte_offset_of(m_input.iterator_at_byte_offset_without_validation(start_offset)));
        return {};
    }

    // Ignore closing quote
    ++m_iterator;

    return m_input.substring_view(start_offset, end_offset - start_offset);
}

template<Integral T>
ErrorOr<T> Utf8GenericLexer::consume_decimal_integer()
{
    using UnsignedT = MakeUnsigned<T>;

    ArmedScopeGuard rollback { [&, rollback_iterator = m_iterator] {
        m_iterator = rollback_iterator;
    } };

    bool has_minus_sign = false;

    if (next_is(static_cast<u32>('+')) || next_is(static_cast<u32>('-'))) {
        if (consume() == static_cast<u32>('-')) {
            has_minus_sign = true;
        }
    }

    auto start_offset = tell();
    while (!is_eof() && is_ascii_digit_unicode(peek())) {
        ++m_iterator;
    }
    auto end_offset = tell();

    if (start_offset == end_offset) {
        return Error::from_errno(EINVAL);
    }

    auto number_view = m_input.substring_view(start_offset, end_offset - start_offset);
    auto maybe_number = StringUtils::convert_to_uint<UnsignedT>(number_view.as_string(), TrimWhitespace::No);
    if (!maybe_number.has_value()) {
        return Error::from_errno(ERANGE);
    }
    auto number = maybe_number.value();

    if (!has_minus_sign) {
        if (NumericLimits<T>::max() < number) { // This is only possible in a signed case.
            return Error::from_errno(ERANGE);
        }

        rollback.disarm();
        return number;
    } else {
        if constexpr (IsUnsigned<T>) {
            if (number == 0) {
                rollback.disarm();
                return 0;
            }
            return Error::from_errno(ERANGE);
        } else {
            static constexpr UnsignedT max_value = static_cast<UnsignedT>(NumericLimits<T>::max()) + 1;
            if (number > max_value) {
                return Error::from_errno(ERANGE);
            }
            rollback.disarm();
            return -number;
        }
    }
}
//
// ErrorOr<String> Utf8GenericLexer::consume_and_unescape_string(u32 escape_char)
// {
//     auto view = consume_quoted_string(escape_char);
//     if (view.is_empty()) {
//         return ""_string;
//     }
//
//     StringBuilder builder;
//     Utf8GenericLexer sub_lexer(view);
//
//     while (!sub_lexer.is_eof()) {
//         auto ch = sub_lexer.consume_escaped_character(escape_char);
//         // Convert code point to UTF-8 bytes
//         if (ch <= 0x7F) {
//             builder.append(static_cast<char>(ch));
//         } else if (ch <= 0x7FF) {
//             builder.append(static_cast<char>(0xC0 | (ch >> 6)));
//             builder.append(static_cast<char>(0x80 | (ch & 0x3F)));
//         } else if (ch <= 0xFFFF) {
//             builder.append(static_cast<char>(0xE0 | (ch >> 12)));
//             builder.append(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
//             builder.append(static_cast<char>(0x80 | (ch & 0x3F)));
//         } else if (ch <= 0x10FFFF) {
//             builder.append(static_cast<char>(0xF0 | (ch >> 18)));
//             builder.append(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
//             builder.append(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
//             builder.append(static_cast<char>(0x80 | (ch & 0x3F)));
//         }
//     }
//
//     return builder.to_string();
// }

auto Utf8GenericLexer::consume_escaped_code_point(bool combine_surrogate_pairs) -> Result<u32, UnicodeEscapeError>
{
    if (!consume_specific(u8"\\u")) {
        return UnicodeEscapeError::MalformedUnicodeEscape;
    }

    if (next_is(static_cast<u32>('{'))) {
        return decode_code_point();
    }
    return decode_single_or_paired_surrogate(combine_surrogate_pairs);
}

auto Utf8GenericLexer::decode_code_point() -> Result<u32, UnicodeEscapeError>
{
    bool starts_with_open_bracket = consume_specific(static_cast<u32>('{'));
    VERIFY(starts_with_open_bracket);

    u32 code_point = 0;

    while (true) {
        auto ch = peek();
        if (!is_ascii_hex_digit(static_cast<char>(ch))) {
            return UnicodeEscapeError::MalformedUnicodeEscape;
        }

        auto new_code_point = (code_point << 4u) | parse_ascii_hex_digit(static_cast<char>(ch));
        if (new_code_point < code_point) {
            return UnicodeEscapeError::UnicodeEscapeOverflow;
        }

        code_point = new_code_point;
        consume();

        if (consume_specific(static_cast<u32>('}'))) {
            break;
        }
    }

    if (is_unicode(code_point)) {
        return code_point;
    }
    return UnicodeEscapeError::UnicodeEscapeOverflow;
}

auto Utf8GenericLexer::decode_single_or_paired_surrogate(bool combine_surrogate_pairs) -> Result<u32, UnicodeEscapeError>
{
    constexpr size_t surrogate_length = 4;

    auto decode_one_surrogate = [&]() -> Optional<u16> {
        u16 surrogate = 0;

        for (size_t i = 0; i < surrogate_length; ++i) {
            auto ch = peek();
            if (!is_ascii_hex_digit(static_cast<char>(ch))) {
                return {};
            }

            surrogate = (surrogate << 4u) | parse_ascii_hex_digit(static_cast<char>(consume()));
        }

        return surrogate;
    };

    auto high_surrogate = decode_one_surrogate();
    if (!high_surrogate.has_value()) {
        return UnicodeEscapeError::MalformedUnicodeEscape;
    }
    if (!Utf16View::is_high_surrogate(*high_surrogate)) {
        return *high_surrogate;
    }
    if (!combine_surrogate_pairs || !consume_specific(u8"\\u")) {
        return *high_surrogate;
    }

    auto low_surrogate = decode_one_surrogate();
    if (!low_surrogate.has_value()) {
        return UnicodeEscapeError::MalformedUnicodeEscape;
    }
    if (Utf16View::is_low_surrogate(*low_surrogate)) {
        return Utf16View::decode_surrogate_pair(*high_surrogate, *low_surrogate);
    }

    retreat(6); // Retreat past the \u and 4 hex digits
    return *high_surrogate;
}

// Explicit template instantiations
template ErrorOr<u8> Utf8GenericLexer::consume_decimal_integer<u8>();
template ErrorOr<i8> Utf8GenericLexer::consume_decimal_integer<i8>();
template ErrorOr<u16> Utf8GenericLexer::consume_decimal_integer<u16>();
template ErrorOr<i16> Utf8GenericLexer::consume_decimal_integer<i16>();
template ErrorOr<u32> Utf8GenericLexer::consume_decimal_integer<u32>();
template ErrorOr<i32> Utf8GenericLexer::consume_decimal_integer<i32>();
template ErrorOr<u64> Utf8GenericLexer::consume_decimal_integer<u64>();
template ErrorOr<i64> Utf8GenericLexer::consume_decimal_integer<i64>();

}
