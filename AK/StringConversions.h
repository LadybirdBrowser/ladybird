/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Optional.h>
#include <AK/StringUtils.h>

namespace AK {

template<typename T>
struct ParseFirstNumberResult {
    T value { 0 };
    size_t characters_parsed { 0 };
};

template<Arithmetic T>
Optional<ParseFirstNumberResult<T>> parse_first_number(StringView, TrimWhitespace = TrimWhitespace::Yes, int base = 10);

template<Arithmetic T>
Optional<ParseFirstNumberResult<T>> parse_first_number(Utf16View const&, TrimWhitespace = TrimWhitespace::Yes, int base = 10);

template<Arithmetic T>
Optional<T> parse_number(StringView, TrimWhitespace = TrimWhitespace::Yes, int base = 10);

template<Arithmetic T>
Optional<T> parse_number(Utf16View const&, TrimWhitespace = TrimWhitespace::Yes, int base = 10);

template<Integral T>
Optional<T> parse_hexadecimal_number(StringView, TrimWhitespace = TrimWhitespace::Yes);

template<Integral T>
Optional<T> parse_hexadecimal_number(Utf16View const&, TrimWhitespace = TrimWhitespace::Yes);

}
