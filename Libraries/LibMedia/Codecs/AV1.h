/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Subsampling.h>

namespace Media::Codecs {

class AV1 {
public:
    enum class Tier : u8 {
        Main,
        High,
    };

    struct OptionalFields {
        bool monochrome { false };
        // The chroma subsampling defaults to 110, which is 4:2:0. Note that Subsampling's own default is 4:4:4.
        Subsampling subsampling { Subsampling::yuv420() };
        u8 chroma_sample_position { 0 };
        CodingIndependentCodePoints cicp {
            ColorPrimaries::BT709,
            TransferCharacteristics::BT709,
            MatrixCoefficients::BT709,
            VideoFullRangeFlag::Studio,
        };

        bool operator==(OptionalFields const&) const = default;
    };

    struct Parameters {
        u8 profile;
        u8 level;
        Tier tier;
        u8 bit_depth;
        OptionalFields optional_fields;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
};

}
