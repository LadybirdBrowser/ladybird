/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/VP9.h>

namespace Media::Codecs {

static bool is_valid_level(u8 level)
{
    return first_is_one_of(level, 10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 52, 60, 61, 62);
}

static bool profile_and_color_config_is_valid(u8 profile, u8 bit_depth, Subsampling subsampling)
{
    auto is_422_or_444 = first_is_one_of(subsampling, Subsampling::yuv422(), Subsampling::yuv444());

    switch (profile) {
    case 0:
        return bit_depth == 8 && subsampling == Subsampling::yuv420();
    case 1:
        return bit_depth == 8 && is_422_or_444;
    case 2:
        return first_is_one_of(bit_depth, 10, 12) && subsampling == Subsampling::yuv420();
    case 3:
        return first_is_one_of(bit_depth, 10, 12) && is_422_or_444;
    default:
        return false;
    }
}

static Optional<Subsampling> subsampling_from_codec_string_value(u8 value)
{
    switch (value) {
    case 0:
    case 1:
        return Subsampling::yuv420();
    case 2:
        return Subsampling::yuv422();
    case 3:
        return Subsampling::yuv444();
    default:
        return {};
    }
}

// https://www.webmproject.org/vp9/mp4/
Optional<VP9::Parameters> VP9::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};
    auto profile = consume_two_digit_decimal(lexer);
    if (!lexer.consume_specific('.'))
        return {};
    auto level = consume_two_digit_decimal(lexer);
    if (!lexer.consume_specific('.'))
        return {};
    auto bit_depth = consume_two_digit_decimal(lexer);
    if (!profile.has_value() || !level.has_value() || !bit_depth.has_value())
        return {};
    if (!is_valid_level(*level) || !first_is_one_of(*bit_depth, 8, 10, 12))
        return {};

    ColorParameters color_parameters;
    if (!lexer.is_eof()) {
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_chroma_subsampling = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_color_primaries = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_transfer_characteristics = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_matrix_coefficients = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto video_full_range = consume_two_digit_decimal(lexer);
        if (!parsed_chroma_subsampling.has_value() || !parsed_color_primaries.has_value() || !parsed_transfer_characteristics.has_value() || !parsed_matrix_coefficients.has_value() || !video_full_range.has_value() || !lexer.is_eof())
            return {};

        auto subsampling = subsampling_from_codec_string_value(*parsed_chroma_subsampling);
        if (!subsampling.has_value())
            return {};
        if (*video_full_range > 1)
            return {};

        auto parsed_cicp = CodingIndependentCodePoints {
            static_cast<ColorPrimaries>(*parsed_color_primaries),
            static_cast<TransferCharacteristics>(*parsed_transfer_characteristics),
            static_cast<MatrixCoefficients>(*parsed_matrix_coefficients),
            *video_full_range == 1 ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio,
        };
        if (!parsed_cicp.is_valid_or_unspecified())
            return {};

        color_parameters.subsampling = *subsampling;
        color_parameters.cicp = parsed_cicp;

        // If matrixCoefficients is 0 (RGB), then chroma subsampling MUST be 3 (4:4:4).
        if (color_parameters.cicp.matrix_coefficients() == MatrixCoefficients::Identity && *subsampling != Subsampling::yuv444())
            return {};
    }

    if (!profile_and_color_config_is_valid(*profile, *bit_depth, color_parameters.subsampling))
        return {};

    return Parameters {
        *profile,
        *level,
        *bit_depth,
        color_parameters,
    };
}

}
