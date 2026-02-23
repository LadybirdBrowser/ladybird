/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FloatingPoint.h>
#include <AK/StringConversions.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <math.h>

#include <fast_float/fast_float.h>
#include <fmt/format.h>

namespace AK {

#define ENUMERATE_INTEGRAL_TYPES    \
    __ENUMERATE_TYPE(i8)            \
    __ENUMERATE_TYPE(i16)           \
    __ENUMERATE_TYPE(i32)           \
    __ENUMERATE_TYPE(long)          \
    __ENUMERATE_TYPE(long long)     \
    __ENUMERATE_TYPE(u8)            \
    __ENUMERATE_TYPE(u16)           \
    __ENUMERATE_TYPE(u32)           \
    __ENUMERATE_TYPE(unsigned long) \
    __ENUMERATE_TYPE(unsigned long long)

#define ENUMERATE_ARITHMETIC_TYPES \
    ENUMERATE_INTEGRAL_TYPES       \
    __ENUMERATE_TYPE(float)        \
    __ENUMERATE_TYPE(double)

template<typename CharType, Arithmetic ValueType>
static constexpr Optional<ParseFirstNumberResult<ValueType>> from_chars(CharType const* string, size_t length, int base)
{
    ValueType value { 0 };

    fast_float::parse_options_t<CharType> options;
    options.base = base;
    options.format |= fast_float::chars_format::no_infnan;

    if constexpr (IsSigned<ValueType> || IsFloatingPoint<ValueType>) {
        options.format |= fast_float::chars_format::allow_leading_plus;
    }

    auto result = fast_float::from_chars_advanced(string, string + length, value, options);

    if constexpr (IsFloatingPoint<ValueType>) {
        if (result.ec == std::errc::result_out_of_range && (__builtin_isinf(value) || value == 0))
            result.ec = {};
    }

    if (result.ec != std::errc {})
        return {};

    return ParseFirstNumberResult { value, static_cast<size_t>(result.ptr - string) };
}

template<Arithmetic T>
Optional<ParseFirstNumberResult<T>> parse_first_number(StringView string, TrimWhitespace trim_whitespace, int base)
{
    if (trim_whitespace == TrimWhitespace::Yes)
        string = StringUtils::trim_whitespace(string, TrimMode::Both);

    return from_chars<char, T>(string.characters_without_null_termination(), string.length(), base);
}

template<Arithmetic T>
Optional<ParseFirstNumberResult<T>> parse_first_number(Utf16View const& string, TrimWhitespace trim_whitespace, int base)
{
    if (string.has_ascii_storage())
        return parse_first_number<T>(string.bytes(), trim_whitespace, base);

    auto trimmed_string = trim_whitespace == TrimWhitespace::Yes ? string.trim_ascii_whitespace() : string;
    return from_chars<char16_t, T>(trimmed_string.utf16_span().data(), trimmed_string.length_in_code_units(), base);
}

#define __ENUMERATE_TYPE(type) \
    template Optional<ParseFirstNumberResult<type>> parse_first_number(StringView, TrimWhitespace, int);
ENUMERATE_ARITHMETIC_TYPES
#undef __ENUMERATE_TYPE

#define __ENUMERATE_TYPE(type) \
    template Optional<ParseFirstNumberResult<type>> parse_first_number(Utf16View const&, TrimWhitespace, int);
ENUMERATE_ARITHMETIC_TYPES
#undef __ENUMERATE_TYPE

template<Arithmetic T>
Optional<T> parse_number(StringView string, TrimWhitespace trim_whitespace, int base)
{
    if (trim_whitespace == TrimWhitespace::Yes)
        string = StringUtils::trim_whitespace(string, TrimMode::Both);

    auto result = parse_first_number<T>(string, TrimWhitespace::No, base);
    if (!result.has_value())
        return {};

    if (result->characters_parsed != string.length())
        return {};

    return result->value;
}

template<Arithmetic T>
Optional<T> parse_number(Utf16View const& string, TrimWhitespace trim_whitespace, int base)
{
    if (string.has_ascii_storage())
        return parse_number<T>(string.bytes(), trim_whitespace, base);

    auto trimmed_string = trim_whitespace == TrimWhitespace::Yes ? string.trim_ascii_whitespace() : string;

    auto result = parse_first_number<T>(trimmed_string, TrimWhitespace::No, base);
    if (!result.has_value())
        return {};

    if (result->characters_parsed != trimmed_string.length_in_code_units())
        return {};

    return result->value;
}

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_number(StringView, TrimWhitespace, int);
ENUMERATE_ARITHMETIC_TYPES
#undef __ENUMERATE_TYPE

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_number(Utf16View const&, TrimWhitespace, int);
ENUMERATE_ARITHMETIC_TYPES
#undef __ENUMERATE_TYPE

template<Integral T>
Optional<T> parse_hexadecimal_number(StringView string, TrimWhitespace trim_whitespace)
{
    return parse_number<T>(string, trim_whitespace, 16);
}

template<Integral T>
Optional<T> parse_hexadecimal_number(Utf16View const& string, TrimWhitespace trim_whitespace)
{
    return parse_number<T>(string, trim_whitespace, 16);
}

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_hexadecimal_number(StringView, TrimWhitespace);
ENUMERATE_INTEGRAL_TYPES
#undef __ENUMERATE_TYPE

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_hexadecimal_number(Utf16View const&, TrimWhitespace);
ENUMERATE_INTEGRAL_TYPES
#undef __ENUMERATE_TYPE

template<FloatingPoint T>
DecimalExponentialForm convert_to_decimal_exponential_form(T value)
{
    ASSERT(!isinf(value));
    ASSERT(!isnan(value));

    FloatExtractor<T> extractor;
    extractor.d = value;

    auto [significand, exponent] = fmt::detail::dragonbox::to_decimal(value);
    return { static_cast<bool>(extractor.sign), significand, exponent };
}

template DecimalExponentialForm convert_to_decimal_exponential_form(float);
template DecimalExponentialForm convert_to_decimal_exponential_form(double);

}
