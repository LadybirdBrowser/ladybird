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

class VP9 {
public:
    struct ColorParameters {
        Subsampling subsampling { Subsampling::yuv420() };
        CodingIndependentCodePoints cicp {
            ColorPrimaries::BT709,
            TransferCharacteristics::BT709,
            MatrixCoefficients::BT709,
            VideoFullRangeFlag::Studio,
        };

        bool operator==(ColorParameters const&) const = default;
    };

    struct Parameters {
        u8 profile { 0 };
        u8 level { 0 };
        u8 bit_depth { 0 };
        ColorParameters color_parameters;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
};

}
