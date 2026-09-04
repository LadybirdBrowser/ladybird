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
        u8 profile { 0 };
        u8 level { 0 };
        Tier tier { Tier::Main };
        u8 bit_depth { 0 };
        OptionalFields optional_fields;

        bool operator==(Parameters const&) const = default;
    };

    struct SequenceHeader {
        Parameters parameters;
        Gfx::IntSize max_frame_size;

        bool operator==(SequenceHeader const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);

    // Reads the sequence header out of a series of OBUs, whether they are a coded frame or a configuration record's
    // own OBUs. Only the coded frames that begin a coded video sequence carry one.
    static MEDIA_API Optional<SequenceHeader> parse_sequence_header(ReadonlyBytes obus);
};

}
