/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/H264.h>

namespace Media::Codecs {

Optional<H264::Parameters> H264::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};

    auto profile_idc = consume_two_digit_hexadecimal(lexer);
    auto constraint_set_flags = consume_two_digit_hexadecimal(lexer);
    auto level_idc = consume_two_digit_hexadecimal(lexer);
    if (!profile_idc.has_value() || !constraint_set_flags.has_value() || !level_idc.has_value() || !lexer.is_eof())
        return {};

    return Parameters {
        *profile_idc,
        *constraint_set_flags,
        *level_idc,
    };
}

}
