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

Optional<H264::Parameters> H264::parse_configuration_record(ReadonlyBytes record)
{
    static constexpr size_t MINIMUM_RECORD_SIZE = 4;
    if (record.size() < MINIMUM_RECORD_SIZE)
        return {};

    // Version 1 is the only version defined, and is what distinguishes a configuration record from Annex B data.
    if (record[0] != 1)
        return {};

    return Parameters {
        record[1],
        record[2],
        record[3],
    };
}

}
