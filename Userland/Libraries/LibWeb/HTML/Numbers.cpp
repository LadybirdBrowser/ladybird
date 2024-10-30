/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <math.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-integers
Optional<i32> parse_integer(StringView string)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer lexer { string };

    // 3. Let sign have the value "positive".
    // NOTE: Skipped, see comment on step 6.

    // 4. Skip ASCII whitespace within input given position.
    lexer.ignore_while(Web::Infra::is_ascii_whitespace);

    // 5. If position is past the end of input, return an error.
    if (lexer.is_eof()) {
        return {};
    }

    // 6. If the character indicated by position (the first character) is a U+002D HYPHEN-MINUS character (-):
    //
    // If we parse a signed integer, then we include the sign character (if present) in the collect step
    // (step 8) and lean on `AK::StringUtils::convert_to_int` to handle it for us.
    size_t start_index = lexer.tell();
    if (lexer.peek() == '-' || lexer.peek() == '+') {
        lexer.consume();
    }

    // 7. If the character indicated by position is not an ASCII digit, then return an error.
    if (!lexer.next_is(is_ascii_digit)) {
        return {};
    }

    // 8. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer. Let value be that integer.
    lexer.consume_while(is_ascii_digit);
    size_t end_index = lexer.tell();
    auto digits = lexer.input().substring_view(start_index, end_index - start_index);
    auto optional_value = AK::StringUtils::convert_to_int<i32>(digits);

    // 9. If sign is "positive", return value, otherwise return the result of subtracting value from zero.
    // NOTE: Skipped, see comment on step 6.

    return optional_value;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-non-negative-integers
Optional<u32> parse_non_negative_integer(StringView string)
{
    // 1. Let input be the string being parsed.
    // 2. Let value be the result of parsing input using the rules for parsing integers.
    //
    // NOTE: Because we call `parse_integer`, we parse all integers as signed. If we need the extra
    //       size that an unsigned integer offers, then this would need to be improved. That said,
    //       I don't think we need to support such large integers at the moment.
    auto optional_value = parse_integer(string);

    // 3. If value is an error, return an error.
    if (!optional_value.has_value()) {
        return {};
    }

    // 4. If value is less than zero, return an error.
    if (optional_value.value() < 0) {
        return {};
    }

    // 5. Return value.
    return static_cast<u32>(optional_value.value());
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-floating-point-number-values
Optional<double> parse_floating_point_number(StringView string)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer lexer { string };

    // 3. Let value have the value 1.
    double value = 1;

    // 4. Let divisor have the value 1.
    double divisor = 1;

    // 5. Let exponent have the value 1.
    i16 exponent = 1;

    // 6. Skip ASCII whitespace within input given position.
    lexer.ignore_while(Web::Infra::is_ascii_whitespace);

    // 7. If position is past the end of input, return an error.
    if (lexer.is_eof()) {
        return {};
    }

    // 8. If the character indicated by position is a U+002D HYPHEN-MINUS character (-):
    if (lexer.next_is('-')) {
        // 8.1. Change value and divisor to −1.
        value = -1;
        divisor = -1;

        // 8.2. Advance position to the next character.
        lexer.consume();

        // 8.3. If position is past the end of input, return an error.
        if (lexer.is_eof()) {
            return {};
        }
    }
    // Otherwise, if the character indicated by position (the first character) is a U+002B PLUS SIGN character (+):
    else if (lexer.next_is('+')) {
        // 8.1. Advance position to the next character. (The "+" is ignored, but it is not conforming.)
        lexer.consume();

        // 8.2. If position is past the end of input, return an error.
        if (lexer.is_eof()) {
            return {};
        }
    }

    // 9. If the character indicated by position is a U+002E FULL STOP (.),
    //    and that is not the last character in input,
    //    and the character after the character indicated by position is an ASCII digit,
    //    then set value to zero and jump to the step labeled fraction.
    if (lexer.next_is('.') && (lexer.tell_remaining() > 1) && is_ascii_digit(lexer.peek(1))) {
        value = 0;
        goto fraction;
    }

    // 10. If the character indicated by position is not an ASCII digit, then return an error.
    if (!lexer.next_is(is_ascii_digit)) {
        return {};
    }

    // 11. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer.
    //     Multiply value by that integer.
    {
        size_t start_index = lexer.tell();
        lexer.consume_while(is_ascii_digit);
        size_t end_index = lexer.tell();
        auto digits = lexer.input().substring_view(start_index, end_index - start_index);
        auto optional_value = AK::StringUtils::convert_to_floating_point<double>(digits, TrimWhitespace::No);
        value *= optional_value.value();
    }

    // 12. If position is past the end of input, jump to the step labeled conversion.
    if (lexer.is_eof()) {
        goto conversion;
    }

fraction: {
    // 13. Fraction: If the character indicated by position is a U+002E FULL STOP (.), run these substeps:
    if (lexer.next_is('.')) {
        // 13.1. Advance position to the next character.
        lexer.consume();

        // 13.2. If position is past the end of input,
        //       or if the character indicated by position is not an ASCII digit,
        //       U+0065 LATIN SMALL LETTER E (e), or U+0045 LATIN CAPITAL LETTER E (E),
        //       then jump to the step labeled conversion.
        if (lexer.is_eof() || (!lexer.next_is(is_ascii_digit) && !lexer.next_is('e') && !lexer.next_is('E'))) {
            goto conversion;
        }

        // 13.3. If the character indicated by position is a U+0065 LATIN SMALL LETTER E character (e) or a U+0045 LATIN CAPITAL LETTER E character (E),
        //       skip the remainder of these substeps.
        if (lexer.next_is('e') || lexer.next_is('E')) {
            goto fraction_exit;
        }

        // fraction_loop:
        while (true) {
            // 13.4. Fraction loop: Multiply divisor by ten.
            divisor *= 10;

            // 13.5. Add the value of the character indicated by position, interpreted as a base-ten digit (0..9) and divided by divisor, to value.
            value += (lexer.peek() - '0') / divisor;

            // 13.6. Advance position to the next character.
            lexer.consume();

            // 13.7. If position is past the end of input, then jump to the step labeled conversion.
            if (lexer.is_eof()) {
                goto conversion;
            }

            // 13.8. If the character indicated by position is an ASCII digit, jump back to the step labeled fraction loop in these substeps.
            if (!lexer.next_is(is_ascii_digit)) {
                break;
            }
        }
    }

fraction_exit:
}

    // 14. If the character indicated by position is U+0065 (e) or a U+0045 (E), then:
    if (lexer.next_is('e') || lexer.next_is('E')) {
        // 14.1. Advance position to the next character.
        lexer.consume();

        // 14.2. If position is past the end of input, then jump to the step labeled conversion.
        if (lexer.is_eof()) {
            goto conversion;
        }

        // 14.3. If the character indicated by position is a U+002D HYPHEN-MINUS character (-):
        if (lexer.next_is('-')) {
            // 14.3.1. Change exponent to −1.
            exponent = -1;

            // 14.3.2. Advance position to the next character.
            lexer.consume();

            // 14.3.3. If position is past the end of input, then jump to the step labeled conversion.
            if (lexer.is_eof()) {
                goto conversion;
            }
        }
        // Otherwise, if the character indicated by position is a U+002B PLUS SIGN character (+):
        else if (lexer.next_is('+')) {
            // 14.3.1. Advance position to the next character.
            lexer.consume();

            // 14.3.2. If position is past the end of input, then jump to the step labeled conversion.
            if (lexer.is_eof()) {
                goto conversion;
            }
        }

        // 14.4. If the character indicated by position is not an ASCII digit, then jump to the step labeled conversion.
        if (!lexer.next_is(is_ascii_digit)) {
            goto conversion;
        }

        // 14.5. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer.
        //       Multiply exponent by that integer.
        {
            size_t start_index = lexer.tell();
            lexer.consume_while(is_ascii_digit);
            size_t end_index = lexer.tell();
            auto digits = lexer.input().substring_view(start_index, end_index - start_index);
            auto optional_value = AK::StringUtils::convert_to_int<i32>(digits);
            exponent *= optional_value.value();
        }

        // 14.6. Multiply value by ten raised to the exponentth power.
        value *= pow(10, exponent);
    }

conversion: {
    // 15. Conversion: Let S be the set of finite IEEE 754 double-precision floating-point values except −0,
    //     but with two special values added: 2^1024 and −2^1024.
    if (!isfinite(value)) {
        return {};
    }
    if ((value == 0) && signbit(value)) {
        return 0;
    }

    // 16. Let rounded-value be the number in S that is closest to value, selecting the number with an even significand if there are two equally close values.
    //     (The two special values 2^1024 and −2^1024 are considered to have even significands for this purpose.)
    double rounded_value = value;

    // 17. If rounded-value is 2^1024 or −2^1024, return an error.
    if (abs(rounded_value) >= pow(2, 1024)) {
        return {};
    }

    // 18. Return rounded-value.
    return rounded_value;
}
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-floating-point-number
bool is_valid_floating_point_number(StringView string)
{
    GenericLexer lexer { string };
    // 1. Optionally, a U+002D HYPHEN-MINUS character (-).
    lexer.consume_specific('-');
    // 2. One or both of the following, in the given order:
    // 2.1. A series of one or more ASCII digits.
    bool has_leading_digits = !lexer.consume_while(is_ascii_digit).is_empty();
    // 2.2. Both of the following, in the given order:
    // 2.2.1. A single U+002E FULL STOP character (.).
    if (lexer.consume_specific('.')) {
        // 2.2.2. A series of one or more ASCII digits.
        if (lexer.consume_while(is_ascii_digit).is_empty())
            return false;
    } else if (!has_leading_digits) {
        // Doesn’t begin with digits, doesn’t begin with a full stop followed by digits.
        return false;
    }
    // 3. Optionally:
    // 3.1. Either a U+0065 LATIN SMALL LETTER E character (e) or a U+0045 LATIN CAPITAL
    //      LETTER E character (E).
    if (lexer.consume_specific('e') || lexer.consume_specific('E')) {
        // 3.2. Optionally, a U+002D HYPHEN-MINUS character (-) or U+002B PLUS SIGN
        //      character (+).
        lexer.consume_specific('-') || lexer.consume_specific('+');
        // 3.3. A series of one or more ASCII digits.
        if (lexer.consume_while(is_ascii_digit).is_empty())
            return false;
    }
    return lexer.tell_remaining() == 0;
}

WebIDL::ExceptionOr<String> convert_non_negative_integer_to_string(JS::Realm& realm, WebIDL::Long value)
{
    if (value < 0)
        return WebIDL::IndexSizeError::create(realm, "The attribute is limited to only non-negative numbers"_string);
    return String::number(value);
}

}
