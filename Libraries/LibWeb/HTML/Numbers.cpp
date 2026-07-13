/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringConversions.h>
#include <AK/Utf16View.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <math.h>

namespace Web::HTML {

static size_t code_unit_length(Utf16View string)
{
    return string.length_in_code_units();
}

static u32 code_unit_at(Utf16View string, size_t index)
{
    return string.code_unit_at(index);
}

template<typename StringType>
static bool is_eof(StringType string, size_t position)
{
    return position >= code_unit_length(string);
}

template<typename StringType>
static Optional<StringType> parse_integer_digits_impl(StringType string)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    size_t position = 0;

    // 3. Let sign have the value "positive".
    // NOTE: Skipped, see comment on step 6.

    // 4. Skip ASCII whitespace within input given position.
    while (!is_eof(string, position) && Web::Infra::is_ascii_whitespace(code_unit_at(string, position)))
        ++position;

    // 5. If position is past the end of input, return an error.
    if (is_eof(string, position))
        return {};

    // 6. If the character indicated by position (the first character) is a U+002D HYPHEN-MINUS character (-):
    //
    // If we parse a signed integer, then we include the sign character (if present) in the collect step
    // (step 8) and lean on `AK::StringUtils::convert_to_int` to handle it for us.
    size_t start_index = position;
    if (code_unit_at(string, position) == '-' || code_unit_at(string, position) == '+')
        ++position;

    // 7. If the character indicated by position is not an ASCII digit, then return an error.
    if (is_eof(string, position) || !is_ascii_digit(code_unit_at(string, position)))
        return {};

    // 8. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer. Let value be that integer.
    // NOTE: Integer conversion is performed by the caller.
    while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
        ++position;

    // 9. If sign is "positive", return value, otherwise return the result of subtracting value from zero.
    // NOTE: Skipped, see comment on step 6.

    return string.substring_view(start_index, position - start_index);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-integers
Optional<Utf16View> parse_integer_digits(Utf16View string)
{
    return parse_integer_digits_impl(string);
}

Optional<i32> parse_integer(Utf16View string)
{
    auto optional_digits = parse_integer_digits_impl(string);
    if (!optional_digits.has_value())
        return {};

    return optional_digits->to_number<i32>(TrimWhitespace::No);
}

template<typename StringType>
static Optional<StringType> parse_non_negative_integer_digits_impl(StringType string)
{
    // 1. Let input be the string being parsed.
    // 2. Let value be the result of parsing input using the rules for parsing integers.
    //
    // NOTE: Because we call `parse_integer`, we parse all integers as signed. If we need the extra
    //       size that an unsigned integer offers, then this would need to be improved. That said,
    //       I don't think we need to support such large integers at the moment.
    auto optional_integer_digits = parse_integer_digits_impl(string);
    // 3. If value is an error, return an error.
    if (!optional_integer_digits.has_value())
        return {};

    // 4. If value is less than zero, return an error.
    if (code_unit_length(*optional_integer_digits) > 1 && code_unit_at(*optional_integer_digits, 0) == '-' && code_unit_at(*optional_integer_digits, 1) != '0')
        return {};

    // 5. Return value.
    // NOTE: Integer conversion is performed by the caller.
    return optional_integer_digits;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-non-negative-integers
Optional<Utf16View> parse_non_negative_integer_digits(Utf16View string)
{
    return parse_non_negative_integer_digits_impl(string);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-non-negative-integers
Optional<u32> parse_non_negative_integer(Utf16View string)
{
    auto optional_digits = parse_non_negative_integer_digits_impl(string);
    if (!optional_digits.has_value())
        return {};

    auto optional_value = optional_digits->template to_number<i64>(TrimWhitespace::No);
    if (!optional_value.has_value() || *optional_value > NumericLimits<u32>::max())
        return {};

    return static_cast<u32>(optional_value.value());
}

template<typename StringType>
static Optional<double> parse_floating_point_number_impl(StringType string)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    size_t position = 0;

    // 3. Let value have the value 1.
    double value = 1;

    // 4. Let divisor have the value 1.
    double divisor = 1;

    // 5. Let exponent have the value 1.
    i16 exponent = 1;

    // 6. Skip ASCII whitespace within input given position.
    while (!is_eof(string, position) && Web::Infra::is_ascii_whitespace(code_unit_at(string, position)))
        ++position;

    // 7. If position is past the end of input, return an error.
    if (is_eof(string, position)) {
        return {};
    }

    // 8. If the character indicated by position is a U+002D HYPHEN-MINUS character (-):
    if (code_unit_at(string, position) == '-') {
        // 8.1. Change value and divisor to −1.
        value = -1;
        divisor = -1;

        // 8.2. Advance position to the next character.
        ++position;

        // 8.3. If position is past the end of input, return an error.
        if (is_eof(string, position)) {
            return {};
        }
    }
    // Otherwise, if the character indicated by position (the first character) is a U+002B PLUS SIGN character (+):
    else if (code_unit_at(string, position) == '+') {
        // 8.1. Advance position to the next character. (The "+" is ignored, but it is not conforming.)
        ++position;

        // 8.2. If position is past the end of input, return an error.
        if (is_eof(string, position)) {
            return {};
        }
    }

    // 9. If the character indicated by position is a U+002E FULL STOP (.),
    //    and that is not the last character in input,
    //    and the character after the character indicated by position is an ASCII digit,
    //    then set value to zero and jump to the step labeled fraction.
    if (code_unit_at(string, position) == '.' && code_unit_length(string) - position > 1 && is_ascii_digit(code_unit_at(string, position + 1))) {
        value = 0;
        goto fraction;
    }

    // 10. If the character indicated by position is not an ASCII digit, then return an error.
    if (!is_ascii_digit(code_unit_at(string, position))) {
        return {};
    }

    // 11. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer.
    //     Multiply value by that integer.
    {
        size_t start_index = position;
        while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
            ++position;
        auto digits = string.substring_view(start_index, position - start_index);
        auto optional_value = digits.template to_number<double>(TrimWhitespace::No);
        value *= optional_value.value();
    }

    // 12. If position is past the end of input, jump to the step labeled conversion.
    if (is_eof(string, position)) {
        goto conversion;
    }

fraction: {
    // 13. Fraction: If the character indicated by position is a U+002E FULL STOP (.), run these substeps:
    if (code_unit_at(string, position) == '.') {
        // 13.1. Advance position to the next character.
        ++position;

        // 13.2. If position is past the end of input,
        //       or if the character indicated by position is not an ASCII digit,
        //       U+0065 LATIN SMALL LETTER E (e), or U+0045 LATIN CAPITAL LETTER E (E),
        //       then jump to the step labeled conversion.
        if (is_eof(string, position) || (!is_ascii_digit(code_unit_at(string, position)) && code_unit_at(string, position) != 'e' && code_unit_at(string, position) != 'E')) {
            goto conversion;
        }

        // 13.3. If the character indicated by position is a U+0065 LATIN SMALL LETTER E character (e) or a U+0045 LATIN CAPITAL LETTER E character (E),
        //       skip the remainder of these substeps.
        if (code_unit_at(string, position) == 'e' || code_unit_at(string, position) == 'E') {
            goto fraction_exit;
        }

        // fraction_loop:
        while (true) {
            // 13.4. Fraction loop: Multiply divisor by ten.
            divisor *= 10;

            // 13.5. Add the value of the character indicated by position, interpreted as a base-ten digit (0..9) and divided by divisor, to value.
            value += (code_unit_at(string, position) - '0') / divisor;

            // 13.6. Advance position to the next character.
            ++position;

            // 13.7. If position is past the end of input, then jump to the step labeled conversion.
            if (is_eof(string, position)) {
                goto conversion;
            }

            // 13.8. If the character indicated by position is an ASCII digit, jump back to the step labeled fraction loop in these substeps.
            if (!is_ascii_digit(code_unit_at(string, position))) {
                break;
            }
        }
    }

fraction_exit:
}

    // 14. If the character indicated by position is U+0065 (e) or a U+0045 (E), then:
    if (code_unit_at(string, position) == 'e' || code_unit_at(string, position) == 'E') {
        // 14.1. Advance position to the next character.
        ++position;

        // 14.2. If position is past the end of input, then jump to the step labeled conversion.
        if (is_eof(string, position)) {
            goto conversion;
        }

        // 14.3. If the character indicated by position is a U+002D HYPHEN-MINUS character (-):
        if (code_unit_at(string, position) == '-') {
            // 14.3.1. Change exponent to −1.
            exponent = -1;

            // 14.3.2. Advance position to the next character.
            ++position;

            // 14.3.3. If position is past the end of input, then jump to the step labeled conversion.
            if (is_eof(string, position)) {
                goto conversion;
            }
        }
        // Otherwise, if the character indicated by position is a U+002B PLUS SIGN character (+):
        else if (code_unit_at(string, position) == '+') {
            // 14.3.1. Advance position to the next character.
            ++position;

            // 14.3.2. If position is past the end of input, then jump to the step labeled conversion.
            if (is_eof(string, position)) {
                goto conversion;
            }
        }

        // 14.4. If the character indicated by position is not an ASCII digit, then jump to the step labeled conversion.
        if (!is_ascii_digit(code_unit_at(string, position))) {
            goto conversion;
        }

        // 14.5. Collect a sequence of code points that are ASCII digits from input given position, and interpret the resulting sequence as a base-ten integer.
        //       Multiply exponent by that integer.
        {
            size_t start_index = position;
            while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
                ++position;
            auto digits = string.substring_view(start_index, position - start_index);
            auto optional_value = digits.template to_number<i32>();
            exponent *= optional_value.value();
        }

        // 14.6. Multiply value by ten raised to the exponentth power.
        value *= pow(10, exponent);
    }

conversion:
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

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-floating-point-number-values
Optional<double> parse_floating_point_number(Utf16View string)
{
    return parse_floating_point_number_impl(string);
}

template<typename StringType>
static bool is_valid_floating_point_number_impl(StringType string)
{
    size_t position = 0;

    // 1. Optionally, a U+002D HYPHEN-MINUS character (-).
    if (!is_eof(string, position) && code_unit_at(string, position) == '-')
        ++position;

    // 2. One or both of the following, in the given order:
    // 2.1. A series of one or more ASCII digits.
    auto leading_digits_start = position;
    while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
        ++position;
    bool has_leading_digits = position != leading_digits_start;

    // 2.2. Both of the following, in the given order:
    // 2.2.1. A single U+002E FULL STOP character (.).
    if (!is_eof(string, position) && code_unit_at(string, position) == '.') {
        ++position;
        // 2.2.2. A series of one or more ASCII digits.
        auto fraction_digits_start = position;
        while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
            ++position;
        if (position == fraction_digits_start)
            return false;
    } else if (!has_leading_digits) {
        // Doesn’t begin with digits, doesn’t begin with a full stop followed by digits.
        return false;
    }
    // 3. Optionally:
    // 3.1. Either a U+0065 LATIN SMALL LETTER E character (e) or a U+0045 LATIN CAPITAL
    //      LETTER E character (E).
    if (!is_eof(string, position) && (code_unit_at(string, position) == 'e' || code_unit_at(string, position) == 'E')) {
        ++position;
        // 3.2. Optionally, a U+002D HYPHEN-MINUS character (-) or U+002B PLUS SIGN
        //      character (+).
        if (!is_eof(string, position) && (code_unit_at(string, position) == '-' || code_unit_at(string, position) == '+'))
            ++position;

        // 3.3. A series of one or more ASCII digits.
        auto exponent_digits_start = position;
        while (!is_eof(string, position) && is_ascii_digit(code_unit_at(string, position)))
            ++position;
        if (position == exponent_digits_start)
            return false;
    }
    return is_eof(string, position);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-floating-point-number
bool is_valid_floating_point_number(Utf16View string)
{
    return is_valid_floating_point_number_impl(string);
}

WebIDL::ExceptionOr<Utf16String> convert_non_negative_integer_to_string(JS::Realm& realm, WebIDL::Long value)
{
    if (value < 0)
        return WebIDL::IndexSizeError::create(realm, "The attribute is limited to only non-negative numbers"_utf16);
    return Utf16String::number(value);
}

}
