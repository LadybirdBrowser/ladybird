/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Export.h>
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

    // What a frame's own header says about its format, which is authoritative over the configuration record and
    // may change from one frame to the next.
    struct FrameHeader {
        u8 profile { 0 };
        u8 bit_depth { 8 };
        Gfx::IntSize size;
        ColorParameters color_parameters;

        bool operator==(FrameHeader const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);

    // Reads the format of the last frame in a coded frame that describes one. A coded frame may pack several VP9
    // frames behind a superframe index, and only key and intra-only frames carry a full description.
    static MEDIA_API Optional<FrameHeader> parse_frame_header(ReadonlyBytes coded_frame);
};

}
