/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringConversions.h>
#include <AK/StringView.h>

#include <fast_float/fast_float.h>

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

template<Arithmetic T>
Optional<ParseFirstNumberResult<T>> parse_first_number(StringView string, TrimWhitespace trim_whitespace, int base)
{
    if (trim_whitespace == TrimWhitespace::Yes)
        string = StringUtils::trim_whitespace(string, TrimMode::Both);

    auto const* begin = string.characters_without_null_termination();
    auto const* end = begin + string.length();
    T value { 0 };

    fast_float::parse_options_t<char> options;
    options.base = base;
    options.format |= fast_float::chars_format::no_infnan;

    if constexpr (IsSigned<T> || IsFloatingPoint<T>) {
        options.format |= fast_float::chars_format::allow_leading_plus;
    }

    auto result = fast_float::from_chars_advanced(begin, end, value, options);

    if constexpr (IsFloatingPoint<T>) {
        if (result.ec == std::errc::result_out_of_range && (__builtin_isinf(value) || value == 0))
            result.ec = {};
    }

    if (result.ec != std::errc {})
        return {};

    return ParseFirstNumberResult { value, static_cast<size_t>(result.ptr - begin) };
}

#define __ENUMERATE_TYPE(type) \
    template Optional<ParseFirstNumberResult<type>> parse_first_number(StringView, TrimWhitespace, int);
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

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_number(StringView, TrimWhitespace, int);
ENUMERATE_ARITHMETIC_TYPES
#undef __ENUMERATE_TYPE

template<Integral T>
Optional<T> parse_hexadecimal_number(StringView string, TrimWhitespace trim_whitespace)
{
    return parse_number<T>(string, trim_whitespace, 16);
}

#define __ENUMERATE_TYPE(type) \
    template Optional<type> parse_hexadecimal_number(StringView, TrimWhitespace);
ENUMERATE_INTEGRAL_TYPES
#undef __ENUMERATE_TYPE

}
