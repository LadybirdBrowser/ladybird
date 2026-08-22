/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/BitReader.h>
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

// Parameters holds the flags indexed by profile, where general_profile_compatibility_flag[i] is bit i.
static u32 reverse_profile_compatibility_flag_bits(u32 flags)
{
    u32 reversed = 0;
    for (size_t bit = 0; bit < 32; bit++)
        reversed |= ((flags >> bit) & 1) << (31 - bit);
    return reversed;
}

Optional<H265::Parameters> H265::parse_configuration_record(ReadonlyBytes record)
{
    BitReader reader { record };

    auto version = reader.read_bits<u8>(8);
    if (version != 1)
        return {};

    Parameters parameters {};
    parameters.profile_space = reader.read_bits<u8>(2);
    parameters.tier_flag = reader.read_bit();
    parameters.profile_idc = reader.read_bits<u8>(5);
    parameters.profile_compatibility_flags = reverse_profile_compatibility_flag_bits(reader.read_bits<u32>(32));
    for (auto& constraint_indicator_flag : parameters.constraint_indicator_flags)
        constraint_indicator_flag = reader.read_bits<u8>(8);
    parameters.level_idc = reader.read_bits<u8>(8);

    if (reader.has_overrun())
        return {};
    return parameters;
}

}
