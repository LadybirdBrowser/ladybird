/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <LibMedia/Codecs/AV1.h>
#include <LibMedia/Codecs/CodecString.h>

namespace Media::Codecs {

static bool next_is_part_of_codec_parameters(GenericLexer const& lexer)
{
    return lexer.next_is('.')
        || lexer.next_is(is_ascii_digit)
        || lexer.next_is(is_any_of("avMH"sv));
}

static bool profile_and_color_config_is_valid(u8 profile, u8 bit_depth, Subsampling subsampling)
{
    switch (profile) {
    case 0:
        return first_is_one_of(bit_depth, 8, 10) && subsampling == Subsampling::yuv420();
    case 1:
        return first_is_one_of(bit_depth, 8, 10) && subsampling == Subsampling::yuv444();
    case 2:
        if (bit_depth == 12)
            return first_is_one_of(subsampling, Subsampling::yuv420(), Subsampling::yuv422(), Subsampling::yuv444());
        return first_is_one_of(bit_depth, 8, 10) && subsampling == Subsampling::yuv422();
    default:
        return false;
    }
}

static bool parameters_are_valid(AV1::Parameters const& parameters)
{
    if (parameters.level > 23 && parameters.level != 31)
        return false;
    if (parameters.tier == AV1::Tier::High && parameters.level < 8)
        return false;

    auto const& optional_fields = parameters.optional_fields;
    if (!profile_and_color_config_is_valid(parameters.profile, parameters.bit_depth, optional_fields.subsampling))
        return false;

    auto is_420 = optional_fields.subsampling == Subsampling::yuv420();
    if (optional_fields.chroma_sample_position != 0 && !is_420)
        return false;

    if (optional_fields.monochrome) {
        if (parameters.profile == 1)
            return false;
        if (!is_420)
            return false;
        if (optional_fields.chroma_sample_position != 0)
            return false;
    }

    if (optional_fields.cicp.matrix_coefficients() == MatrixCoefficients::Identity) {
        if (optional_fields.subsampling != Subsampling::yuv444())
            return false;
        if (optional_fields.cicp.video_full_range_flag() != VideoFullRangeFlag::Full)
            return false;
    }

    return true;
}

// https://aomediacodec.github.io/av1-isobmff/#codecsparam
Optional<AV1::Parameters> AV1::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};
    auto profile = consume_one_digit_decimal(lexer);
    if (!lexer.consume_specific('.'))
        return {};
    auto level = consume_two_digit_decimal(lexer);
    if (!profile.has_value() || !level.has_value() || lexer.is_eof())
        return {};

    Tier tier;
    if (lexer.consume_specific('M'))
        tier = Tier::Main;
    else if (lexer.consume_specific('H'))
        tier = Tier::High;
    else
        return {};

    if (!lexer.consume_specific('.'))
        return {};
    auto bit_depth = consume_two_digit_decimal(lexer);
    if (!bit_depth.has_value())
        return {};

    OptionalFields optional_fields;
    if (next_is_part_of_codec_parameters(lexer)) {
        if (!lexer.consume_specific('.'))
            return {};
        auto monochrome = consume_one_digit_decimal(lexer);
        if (!monochrome.has_value() || *monochrome > 1)
            return {};

        if (!lexer.consume_specific('.'))
            return {};
        auto chroma_subsampling_x = consume_one_digit_decimal(lexer);
        auto chroma_subsampling_y = consume_one_digit_decimal(lexer);
        auto chroma_sample_position = consume_one_digit_decimal(lexer);
        if (!chroma_subsampling_x.has_value() || !chroma_subsampling_y.has_value() || !chroma_sample_position.has_value())
            return {};
        if (*chroma_subsampling_x > 1 || *chroma_subsampling_y > 1)
            return {};
        if (*chroma_sample_position > 2)
            return {};

        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_color_primaries = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_transfer_characteristics = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_matrix_coefficients = consume_two_digit_decimal(lexer);
        if (!parsed_color_primaries.has_value() || !parsed_transfer_characteristics.has_value() || !parsed_matrix_coefficients.has_value())
            return {};

        if (!lexer.consume_specific('.'))
            return {};
        auto video_full_range = consume_one_digit_decimal(lexer);
        if (!video_full_range.has_value() || *video_full_range > 1)
            return {};

        if (next_is_part_of_codec_parameters(lexer))
            return {};

        auto parsed_cicp = CodingIndependentCodePoints {
            static_cast<ColorPrimaries>(*parsed_color_primaries),
            static_cast<TransferCharacteristics>(*parsed_transfer_characteristics),
            static_cast<MatrixCoefficients>(*parsed_matrix_coefficients),
            *video_full_range == 1 ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio,
        };
        if (!parsed_cicp.is_valid_or_unspecified())
            return {};

        optional_fields.monochrome = *monochrome == 1;
        optional_fields.subsampling = Subsampling { *chroma_subsampling_x == 1, *chroma_subsampling_y == 1 };
        optional_fields.chroma_sample_position = *chroma_sample_position;
        optional_fields.cicp = parsed_cicp;
    }

    Parameters parameters {
        *profile,
        *level,
        tier,
        *bit_depth,
        optional_fields,
    };
    if (!parameters_are_valid(parameters))
        return {};
    return parameters;
}

}
