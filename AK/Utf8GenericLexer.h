/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Result.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf8View.h>

namespace AK {

class Utf8GenericLexer {
public:
    explicit Utf8GenericLexer(Utf8View input)
        : m_input(input)
        , m_iterator(input.begin())
    {
    }

    explicit Utf8GenericLexer(StringView input)
        : Utf8GenericLexer(Utf8View(input))
    {
    }

    template<size_t N>
    explicit Utf8GenericLexer(char8_t const (&input)[N])
        : Utf8GenericLexer(Utf8View(input))
    {
    }

    size_t tell() const
    {
        return m_input.byte_offset_of(m_iterator);
    }

    size_t tell_remaining() const
    {
        return m_input.byte_length() - tell();
    }

    Utf8View remaining() const
    {
        return m_input.substring_view(tell());
    }

    Utf8View input() const
    {
        return m_input;
    }

    bool is_eof() const
    {
        return m_iterator == m_input.end();
    }

    u32 peek(size_t const offset = 0) const
    {
        auto it = m_iterator;
        for (size_t i = 0; i < offset && it != m_input.end(); ++i) {
            ++it;
        }
        return (it != m_input.end()) ? *it : 0;
    }

    Optional<Utf8View> peek_string(size_t const code_point_count, size_t const offset = 0) const
    {
        auto it = m_iterator;
        for (size_t i = 0; i < offset && it != m_input.end(); ++i) {
            ++it;
        }

        if (it == m_input.end()) {
            return {};
        }

        auto const start_byte_offset = m_input.byte_offset_of(it);
        size_t code_points_counted = 0;

        while (it != m_input.end() && code_points_counted < code_point_count) {
            ++it;
            ++code_points_counted;
        }

        if (code_points_counted < code_point_count) {
            return {};
        }

        auto end_byte_offset = (it == m_input.end()) ? m_input.byte_length() : m_input.byte_offset_of(it);
        return m_input.substring_view(start_byte_offset, end_byte_offset - start_byte_offset);
    }

    bool next_is(u32 expected) const
    {
        return peek() == expected;
    }

    bool next_is(Utf8View expected) const
    {
        auto peek_view = peek_string(expected.length());
        if (!peek_view.has_value()) {
            return false;
        }

        auto expected_it = expected.begin();
        auto peek_it = peek_view->begin();

        while (expected_it != expected.end() && peek_it != peek_view->end()) {
            if (*expected_it != *peek_it) {
                return false;
            }
            ++expected_it;
            ++peek_it;
        }

        return expected_it == expected.end() && peek_it == peek_view->end();
    }

    template<size_t N>
    bool next_is(char8_t const (&expected)[N]) const
    {
        return next_is(StringView(reinterpret_cast<char const*>(expected), N));
    }

    bool next_is(StringView expected) const
    {
        return next_is(Utf8View(expected));
    }

    void retreat()
    {
        VERIFY(m_iterator != m_input.begin());
        auto current_offset = m_input.byte_offset_of(m_iterator);
        if (current_offset == 0) {
            return;
        }

        size_t prev_offset = current_offset - 1;
        while (prev_offset > 0) {
            auto test_it = m_input.iterator_at_byte_offset_without_validation(prev_offset);
            auto next_it = test_it;
            ++next_it;
            if (m_input.byte_offset_of(next_it) == current_offset) {
                m_iterator = test_it;
                return;
            }
            --prev_offset;
        }

        m_iterator = m_input.begin();
    }

    void retreat(size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            retreat();
        }
    }

    u32 consume()
    {
        VERIFY(!is_eof());
        u32 code_point = *m_iterator;
        ++m_iterator;
        return code_point;
    }

    template<typename T>
    bool consume_specific(T const& next)
    {
        if (!next_is(next)) {
            return false;
        }

        if constexpr (requires { next.length(); }) {
            ignore(next.length());
        } else if constexpr (IsSame<T, u32>) {
            ignore(1);
        } else {
            static_assert(false, "Unsupported type for consume_specific");
        }
        return true;
    }

    bool consume_specific(String const& next)
    {
        return consume_specific(Utf8View(next.bytes_as_string_view()));
    }

    template<size_t N>
    bool consume_specific(char8_t const (&next)[N])
    {
        return consume_specific(Utf8View(next));
    }

    // FIXME: Get this working
    // u32 consume_escaped_character(u32 escape_char = U'\\', Utf8View escape_map = "n\nr\rt\tb\bf\f"sv) {
    //     if (!consume_specific(escape_char)) {
    //         return consume();
    //     }
    //
    //     auto c = consume();
    //
    //     auto escape_it = escape_map.begin();
    //     while (escape_it != escape_map.end()) {
    //         auto key = *escape_it;
    //         ++escape_it;
    //         if (escape_it == escape_map.end()) break;
    //         auto value = *escape_it;
    //         ++escape_it;
    //
    //         if (c == key) {
    //             return value;
    //         }
    //     }
    //
    //     return c;
    // }

    Utf8View consume(size_t code_point_count);

    Utf8View consume_all();

    Utf8View consume_line();

    Utf8View consume_until(u32);
    Utf8View consume_until(Utf8View);
    template<size_t N>
    Utf8View consume_until(char8_t const (&stop)[N])
    {
        return consume_until(Utf8View(stop));
    }
    Utf8View consume_quoted_string(u32 escape_char = 0);
    ErrorOr<String> consume_and_unescape_string(u32 escape_char = U'\\');

    template<Integral T>
    ErrorOr<T> consume_decimal_integer();

    enum class UnicodeEscapeError : u8 {
        MalformedUnicodeEscape,
        UnicodeEscapeOverflow,
    };

    Result<u32, UnicodeEscapeError> consume_escaped_code_point(bool combine_surrogate_pairs = true);

    void ignore(size_t code_point_count = 1)
    {
        for (size_t i = 0; i < code_point_count && !is_eof(); ++i) {
            ++m_iterator;
        }
    }

    void ignore_until(u32 stop)
    {
        while (!is_eof() && peek() != stop) {
            ++m_iterator;
        }
    }

    void ignore_until(Utf8View stop)
    {
        while (!is_eof() && !next_is(stop)) {
            ++m_iterator;
        }
    }

    template<size_t N>
    void ignore_until(char8_t const (&stop)[N])
    {
        ignore_until(Utf8View(stop));
    }

    template<typename TPredicate>
    bool next_is(TPredicate pred) const
    {
        return pred(peek());
    }

    template<typename TPredicate>
    Utf8View consume_while(TPredicate pred)
    {
        auto start_offset = tell();
        while (!is_eof() && pred(peek())) {
            ++m_iterator;
        }
        auto end_offset = tell();
        return m_input.substring_view(start_offset, end_offset - start_offset);
    }

    template<typename TPredicate>
    Utf8View consume_until(TPredicate pred)
    {
        auto start_offset = tell();
        while (!is_eof() && !pred(peek())) {
            ++m_iterator;
        }
        auto end_offset = tell();
        return m_input.substring_view(start_offset, end_offset - start_offset);
    }

    template<typename TPredicate>
    void ignore_while(TPredicate pred)
    {
        while (!is_eof() && pred(peek())) {
            ++m_iterator;
        }
    }

    template<typename TPredicate>
    void ignore_until(TPredicate pred)
    {
        while (!is_eof() && !pred(peek())) {
            ++m_iterator;
        }
    }

protected:
    Result<u32, UnicodeEscapeError> decode_code_point();
    Result<u32, UnicodeEscapeError> decode_single_or_paired_surrogate(bool combine_surrogate_pairs = true);

    Utf8View m_input;
    Utf8CodePointIterator m_iterator;
};

constexpr auto is_any_of_utf8(Utf8View values)
{
    return [values](u32 c) {
        return values.contains(c);
    };
}

constexpr auto is_not_any_of_utf8(Utf8View values)
{
    return [values](u32 c) {
        return !values.contains(c);
    };
}

constexpr auto is_whitespace_unicode = [](u32 c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == U'\v' || c == U'\u00A0' || // Non-breaking space
        (c >= U'\u2000' && c <= U'\u200A') ||                                                                   // Various Unicode spaces
        c == U'\u2028' || c == U'\u2029';                                                                       // Line/paragraph separators
};

constexpr auto is_newline_unicode = [](u32 const c) {
    return c == U'\n' || c == U'\r' || c == U'\u2028' || c == U'\u2029';
};

constexpr auto is_ascii_digit_unicode = [](u32 const c) {
    return c >= U'0' && c <= U'9';
};

constexpr auto is_ascii_alpha_unicode = [](u32 const c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
};

constexpr auto is_ascii_alnum_unicode = [](u32 const c) {
    return is_ascii_alpha_unicode(c) || is_ascii_digit_unicode(c);
};

}

#if USING_AK_GLOBALLY
// FIXME: This can remove the _utf8/_unicode suffixes when GenericLexer is fully replaced with UTF8GenericLexer
using AK::is_any_of_utf8;
using AK::is_ascii_alnum_unicode;
using AK::is_ascii_alpha_unicode;
using AK::is_ascii_digit_unicode;
using AK::is_newline_unicode;
using AK::is_not_any_of_utf8;
using AK::is_whitespace_unicode;
using AK::Utf8GenericLexer;
#endif
