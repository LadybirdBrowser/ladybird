/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>

namespace Media::Codecs {

inline Optional<u8> consume_one_digit_decimal(GenericLexer& lexer)
{
    auto digits = lexer.consume(1);
    if (digits.length() != 1)
        return {};
    return digits.to_number<u8>(TrimWhitespace::No);
}

inline Optional<u8> consume_two_digit_decimal(GenericLexer& lexer)
{
    auto digits = lexer.consume(2);
    if (digits.length() != 2)
        return {};
    return digits.to_number<u8>(TrimWhitespace::No);
}

inline Optional<u8> consume_two_digit_hexadecimal(GenericLexer& lexer)
{
    auto digits = lexer.consume(2);
    if (digits.length() != 2)
        return {};
    return digits.to_number<u8>(TrimWhitespace::No, 16);
}

}
