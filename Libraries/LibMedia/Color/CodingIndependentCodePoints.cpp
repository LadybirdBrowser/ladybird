/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::CodingIndependentCodePoints const& cicp)
{
    TRY(encoder.encode(cicp.color_primaries()));
    TRY(encoder.encode(cicp.transfer_characteristics()));
    TRY(encoder.encode(cicp.matrix_coefficients()));
    TRY(encoder.encode(cicp.video_full_range_flag()));
    return {};
}

template<>
ErrorOr<Media::CodingIndependentCodePoints> decode(Decoder& decoder)
{
    Media::CodingIndependentCodePoints cicp {
        TRY(decoder.decode<Media::ColorPrimaries>()),
        TRY(decoder.decode<Media::TransferCharacteristics>()),
        TRY(decoder.decode<Media::MatrixCoefficients>()),
        TRY(decoder.decode<Media::VideoFullRangeFlag>()),
    };
    if (!cicp.is_valid_or_unspecified())
        return Error::from_string_literal("IPC: Invalid CICP metadata");
    return cicp;
}

}
