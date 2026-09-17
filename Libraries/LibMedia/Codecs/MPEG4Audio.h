/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibMedia/Codecs/AAC.h>

namespace Media::Codecs {

class MPEG4Audio {
public:
    // https://mp4ra.org/registered-types/object-types
    static constexpr u8 MPEG2_BACKWARDS_COMPATIBLE_AUDIO_OBJECT_TYPE_INDICATION = 0x69;
    static constexpr u8 MPEG1_AUDIO_OBJECT_TYPE_INDICATION = 0x6B;

    struct MP3 { };
    using Codec = Variant<AAC::Parameters, MP3>;

    static Optional<Codec> parse_codec_parameters(GenericLexer&);
};

}
