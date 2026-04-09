/*
 * Copyright (c) 2020, Benoit Lormeau <blormeau@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RedBlackTree.h>
#include <AK/Result.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>

namespace AK {

constexpr auto is_any_of(StringView values)
{
    return [values](auto c) { return values.contains(c); };
}

constexpr auto is_not_any_of(StringView values)
{
    return [values](auto c) { return !values.contains(c); };
}

constexpr auto is_path_separator = is_any_of("/\\"sv);
constexpr auto is_quote = is_any_of("'\""sv);

enum class UnicodeEscapeError {
    MalformedUnicodeEscape,
    UnicodeEscapeOverflow,
};

namespace Detail {

template<typename CharType>
class GenericLexer {
    static_assert(IsOneOf<CharType, char, char16_t>);

public:
    using ViewType = Detail::Conditional<IsSame<CharType, char>, StringView, Utf16View>;

    constexpr explicit GenericLexer(ViewType input)
        : m_input(input)
    {
    }

    constexpr size_t tell() const { return m_index; }
    constexpr size_t tell_remaining() const { return input_length() - m_index; }

    constexpr ViewType remaining() const { return m_input.substring_view(m_index); }
    constexpr ViewType input() const { return m_input; }

    constexpr bool is_eof() const { return m_index >= input_length(); }

    constexpr CharType peek(size_t offset = 0) const
    {
        return (m_index + offset < input_length()) ? code_unit_at(m_index + offset) : '\0';
    }

    constexpr Optional<ViewType> peek_string(size_t length, size_t offset = 0) const
    {
        if (m_index + offset + length > input_length())
            return {};
        return m_input.substring_view(m_index + offset, length);
    }

    constexpr bool next_is(CharType expected) const
    {
        return peek() == expected;
    }

    constexpr bool next_is(char expected) const
    requires(IsSame<CharType, char16_t>)
    {
        return peek() == expected;
    }

    constexpr bool next_is(ViewType expected) const
    {
        size_t length = 0;

        if constexpr (IsSame<CharType, char16_t>)
            length = expected.length_in_code_units();
        else
            length = expected.length();

        return peek_string(length) == expected;
    }

    constexpr bool next_is(StringView expected) const
    requires(IsSame<CharType, char16_t>)
    {
        return peek_string(expected.length()) == expected;
    }

    constexpr void retreat()
    {
        VERIFY(m_index > 0);
        --m_index;
    }

    constexpr void retreat(size_t count)
    {
        VERIFY(m_index >= count);
        m_index -= count;
    }

    constexpr CharType consume()
    {
        VERIFY(!is_eof());
        return code_unit_at(m_index++);
    }

    constexpr bool consume_specific(CharType next)
    {
        if (!next_is(next))
            return false;

        ignore();
        return true;
    }

    constexpr bool consume_specific(char next)
    requires(IsSame<CharType, char16_t>)
    {
        return consume_specific(static_cast<char16_t>(next));
    }

    constexpr bool consume_specific(ViewType next)
    {
        if (!next_is(next))
            return false;

        if constexpr (IsSame<CharType, char16_t>)
            ignore(next.length_in_code_units());
        else
            ignore(next.length());

        return true;
    }

    constexpr bool consume_specific(StringView next)
    requires(IsSame<CharType, char16_t>)
    {
        if (!next_is(next))
            return false;

        ignore(next.length());
        return true;
    }

    constexpr CharType consume_escaped_character(CharType escape_char = '\\', StringView escape_map = "n\nr\rt\tb\bf\f"sv)
    {
        if (!consume_specific(escape_char))
            return consume();

        auto c = consume();

        for (size_t i = 0; i < escape_map.length(); i += 2) {
            if (c == escape_map[i])
                return escape_map[i + 1];
        }

        return c;
    }

    // Consume a number of characters
    constexpr ViewType consume(size_t count)
    {
        auto start = m_index;
        auto length = min(count, input_length() - m_index);
        m_index += length;

        return m_input.substring_view(start, length);
    }

    // Consume the rest of the input
    constexpr ViewType consume_all()
    {
        auto rest = m_input.substring_view(m_index, input_length() - m_index);
        m_index = input_length();
        return rest;
    }

    // Consume until a new line is found
    constexpr ViewType consume_line()
    {
        auto start = m_index;
        while (!is_eof() && peek() != '\r' && peek() != '\n')
            m_index++;

        auto length = m_index - start;
        consume_specific('\r');
        consume_specific('\n');

        return m_input.substring_view(start, length);
    }

    // Consume and return characters until `stop` is peeked
    constexpr ViewType consume_until(CharType stop)
    {
        auto start = m_index;
        while (!is_eof() && peek() != stop)
            m_index++;

        auto length = m_index - start;
        return m_input.substring_view(start, length);
    }

    constexpr ViewType consume_until(char stop)
    requires(IsSame<CharType, char16_t>)
    {
        return consume_until(static_cast<char16_t>(stop));
    }

    // Consume and return characters until the string `stop` is found
    constexpr ViewType consume_until(ViewType stop)
    {
        auto start = m_index;
        while (!is_eof() && !next_is(stop))
            m_index++;

        auto length = m_index - start;
        return m_input.substring_view(start, length);
    }

    // Consume a string surrounded by single or double quotes. The returned ViewType does not include the quotes. An
    // escape character can be provided to capture the enclosing quotes. Please note that the escape character will
    // still be in the resulting ViewType.
    constexpr ViewType consume_quoted_string(CharType escape_char = 0)
    {
        if (!next_is(is_quote))
            return {};

        auto quote_char = consume();
        auto start = m_index;
        while (!is_eof()) {
            if (next_is(escape_char))
                m_index++;
            else if (next_is(quote_char))
                break;
            m_index++;
        }
        auto length = m_index - start;

        if (peek() != quote_char) {
            // Restore the index in case the string is unterminated
            m_index = start - 1;
            return {};
        }

        // Ignore closing quote
        ignore();

        return m_input.substring_view(start, length);
    }

    template<Integral T>
    ErrorOr<T> consume_decimal_integer()
    {
        using UnsignedT = MakeUnsigned<T>;

        ArmedScopeGuard rollback { [&, rollback_position = m_index]() {
            m_index = rollback_position;
        } };

        bool has_minus_sign = false;

        if (next_is('+') || next_is('-'))
            if (consume() == '-')
                has_minus_sign = true;

        auto number_view = consume_while(is_ascii_digit);
        if (number_view.is_empty())
            return Error::from_errno(EINVAL);

        auto maybe_number = number_view.template to_number<UnsignedT>(TrimWhitespace::No);
        if (!maybe_number.has_value())
            return Error::from_errno(ERANGE);
        auto number = maybe_number.value();

        if (!has_minus_sign) {
            if (NumericLimits<T>::max() < number) // This is only possible in a signed case.
                return Error::from_errno(ERANGE);

            rollback.disarm();
            return number;
        }

        if constexpr (IsUnsigned<T>) {
            if (number != 0)
                return Error::from_errno(ERANGE);

            rollback.disarm();
            return 0;
        } else {
            static constexpr UnsignedT max_value = static_cast<UnsignedT>(NumericLimits<T>::max()) + 1;
            if (number > max_value)
                return Error::from_errno(ERANGE);

            rollback.disarm();
            return -number;
        }
    }

    Result<u32, UnicodeEscapeError> consume_escaped_code_point(bool combine_surrogate_pairs = true)
    {
        if (!consume_specific("\\u"sv))
            return UnicodeEscapeError::MalformedUnicodeEscape;

        if (next_is('{'))
            return decode_code_point();
        return decode_single_or_paired_surrogate(combine_surrogate_pairs);
    }

    constexpr void ignore(size_t count = 1)
    {
        count = min(count, input_length() - m_index);
        m_index += count;
    }

    constexpr void ignore_until(CharType stop)
    {
        while (!is_eof() && peek() != stop)
            ++m_index;
    }

    constexpr void ignore_until(char stop)
    requires(IsSame<CharType, char16_t>)
    {
        return ignore_until(static_cast<char16_t>(stop));
    }

    // Conditions are used to match arbitrary characters. You can use lambdas, ctype functions, or is_any_of() and its
    // derivatives (see below).
    //
    // A few examples:
    //   - `if (lexer.next_is(isdigit))`
    //   - `auto name = lexer.consume_while([](char c) { return isalnum(c) || c == '_'; });`
    //   - `lexer.ignore_until(is_any_of("<^>"));`

    // Test the next character against a Condition
    template<typename TPredicate>
    constexpr bool next_is(TPredicate pred) const
    {
        return pred(peek());
    }

    // Consume and return characters while `pred` returns true
    template<typename TPredicate>
    constexpr ViewType consume_while(TPredicate pred)
    {
        auto start = m_index;
        while (!is_eof() && pred(peek()))
            ++m_index;

        auto length = m_index - start;
        return m_input.substring_view(start, length);
    }

    // Consume and return characters until `pred` return true
    template<typename TPredicate>
    constexpr ViewType consume_until(TPredicate pred)
    {
        auto start = m_index;
        while (!is_eof() && !pred(peek()))
            ++m_index;

        auto length = m_index - start;
        return m_input.substring_view(start, length);
    }

    template<typename TPredicate>
    constexpr bool consume_specific_with_predicate(TPredicate pred)
    {
        if (is_eof() || !pred(peek()))
            return false;

        ignore();
        return true;
    }

    // Ignore characters while `pred` returns true
    template<typename TPredicate>
    constexpr void ignore_while(TPredicate pred)
    {
        while (!is_eof() && pred(peek()))
            ++m_index;
    }

    // Ignore characters until `pred` returns true
    template<typename TPredicate>
    constexpr void ignore_until(TPredicate pred)
    {
        while (!is_eof() && !pred(peek()))
            ++m_index;
    }

protected:
    Result<u32, UnicodeEscapeError> decode_code_point()
    {
        bool starts_with_open_bracket = consume_specific('{');
        VERIFY(starts_with_open_bracket);

        u32 code_point = 0;

        while (true) {
            if (!next_is(is_ascii_hex_digit))
                return UnicodeEscapeError::MalformedUnicodeEscape;

            auto new_code_point = (code_point << 4u) | parse_ascii_hex_digit(consume());
            if (new_code_point < code_point)
                return UnicodeEscapeError::UnicodeEscapeOverflow;

            code_point = new_code_point;
            if (consume_specific('}'))
                break;
        }

        if (is_unicode(code_point))
            return code_point;
        return UnicodeEscapeError::UnicodeEscapeOverflow;
    }

    Result<u32, UnicodeEscapeError> decode_single_or_paired_surrogate(bool combine_surrogate_pairs = true)
    {
        constexpr size_t surrogate_length = 4;

        auto decode_one_surrogate = [&]() -> Optional<u16> {
            u16 surrogate = 0;

            for (size_t i = 0; i < surrogate_length; ++i) {
                if (!next_is(is_ascii_hex_digit))
                    return {};

                surrogate = (surrogate << 4u) | parse_ascii_hex_digit(consume());
            }

            return surrogate;
        };

        auto high_surrogate = decode_one_surrogate();
        if (!high_surrogate.has_value())
            return UnicodeEscapeError::MalformedUnicodeEscape;
        if (!UnicodeUtils::is_utf16_high_surrogate(*high_surrogate))
            return *high_surrogate;
        if (!combine_surrogate_pairs || !consume_specific("\\u"sv))
            return *high_surrogate;

        if (next_is('{')) {
            retreat(2);
            return *high_surrogate;
        }

        auto low_surrogate = decode_one_surrogate();
        if (!low_surrogate.has_value())
            return UnicodeEscapeError::MalformedUnicodeEscape;
        if (UnicodeUtils::is_utf16_low_surrogate(*low_surrogate))
            return UnicodeUtils::decode_utf16_surrogate_pair(*high_surrogate, *low_surrogate);

        retreat(6);
        return *high_surrogate;
    }

    constexpr size_t input_length() const
    {
        if constexpr (IsSame<CharType, char16_t>)
            return m_input.length_in_code_units();
        else
            return m_input.length();
    }

    constexpr CharType code_unit_at(size_t index) const
    {
        if constexpr (IsSame<CharType, char16_t>)
            return m_input.code_unit_at(index);
        else
            return m_input[index];
    }

    ViewType m_input;
    size_t m_index { 0 };
};

}

class LineTrackingLexer : public GenericLexer {
public:
    struct Position {
        size_t offset { 0 };
        size_t line { 0 };
        size_t column { 0 };
    };

    LineTrackingLexer(StringView input, Position start_position)
        : GenericLexer(input)
        , m_first_line_start_position(start_position)
        , m_line_start_positions(make<RedBlackTree<size_t, size_t>>())
    {
        m_line_start_positions->insert(0, 0);
        auto first_newline = input.find('\n').map([](auto x) { return x + 1; }).value_or(input.length());
        m_line_start_positions->insert(first_newline, 1);
        m_largest_known_line_start_position = first_newline;
    }

    LineTrackingLexer(StringView input)
        : LineTrackingLexer(input, { 0, 1, 1 })
    {
    }

    Position position_for(size_t) const;
    Position current_position() const { return position_for(m_index); }

protected:
    Position m_first_line_start_position;
    mutable NonnullOwnPtr<RedBlackTree<size_t, size_t>> m_line_start_positions; // offset -> line index
    mutable size_t m_largest_known_line_start_position { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::GenericLexer;
using AK::is_any_of;
using AK::is_path_separator;
using AK::is_quote;
using AK::LineTrackingLexer;
#endif
