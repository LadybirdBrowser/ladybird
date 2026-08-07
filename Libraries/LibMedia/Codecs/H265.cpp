/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/H265.h>

namespace Media::Codecs {

Optional<H265::Parameters> H265::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};

    Parameters parameters {};
    if (lexer.next_is(is_any_of("ABC"sv)))
        parameters.profile_space = lexer.consume() - 'A' + 1;

    auto profile_idc = lexer.consume_while(is_ascii_digit);
    if (profile_idc.is_empty() || profile_idc.length() > 2)
        return {};
    auto maybe_profile_idc = profile_idc.to_number<u8>(TrimWhitespace::No);
    if (!maybe_profile_idc.has_value() || *maybe_profile_idc > 31)
        return {};
    parameters.profile_idc = *maybe_profile_idc;

    if (!lexer.consume_specific('.'))
        return {};

    auto profile_compatibility_flags = lexer.consume_while(is_ascii_hex_digit);
    if (profile_compatibility_flags.is_empty() || profile_compatibility_flags.length() > 8)
        return {};
    auto maybe_profile_compatibility_flags = profile_compatibility_flags.to_number<u32>(TrimWhitespace::No, 16);
    if (!maybe_profile_compatibility_flags.has_value())
        return {};
    parameters.profile_compatibility_flags = *maybe_profile_compatibility_flags;

    if (!lexer.consume_specific('.'))
        return {};

    if (!lexer.next_is(is_any_of("LH"sv)))
        return {};
    parameters.tier_flag = lexer.consume() == 'H';

    auto level_idc = lexer.consume_while(is_ascii_digit);
    if (level_idc.is_empty() || level_idc.length() > 3)
        return {};
    auto maybe_level_idc = level_idc.to_number<u8>(TrimWhitespace::No);
    if (!maybe_level_idc.has_value())
        return {};
    parameters.level_idc = *maybe_level_idc;

    for (size_t index = 0; lexer.consume_specific('.'); index++) {
        if (index == parameters.constraint_indicator_flags.size())
            return {};
        auto digits = lexer.consume_while(is_ascii_hex_digit);
        if (digits.is_empty() || digits.length() > 2)
            return {};
        auto flag = digits.to_number<u8>(TrimWhitespace::No, 16);
        if (!flag.has_value())
            return {};
        parameters.constraint_indicator_flags[index] = *flag;
    }

    if (!lexer.is_eof())
        return {};
    return parameters;
}

}
