/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/MPEG4Audio.h>

namespace Media::Codecs {

Optional<MPEG4Audio::Codec> MPEG4Audio::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};

    auto object_type_indication = consume_two_digit_hexadecimal(lexer);
    if (!object_type_indication.has_value())
        return {};

    if (*object_type_indication == 0x40) {
        Optional<u32> audio_object_type;
        if (!lexer.is_eof()) {
            if (!lexer.consume_specific('.') || !lexer.next_is(is_ascii_digit))
                return {};
            auto result = lexer.consume_decimal_integer<u32>();
            if (result.is_error())
                return {};
            audio_object_type = result.release_value();
        }
        if (!lexer.is_eof())
            return {};
        return Codec { AAC::Parameters { *object_type_indication, audio_object_type } };
    }

    if (first_is_one_of(*object_type_indication, 0x66, 0x67, 0x68)) {
        if (!lexer.is_eof())
            return {};
        return Codec { AAC::Parameters { *object_type_indication, {} } };
    }

    if (first_is_one_of(*object_type_indication, 0x69, 0x6B)) {
        if (!lexer.is_eof())
            return {};
        return Codec { MP3 {} };
    }

    return {};
}

}
