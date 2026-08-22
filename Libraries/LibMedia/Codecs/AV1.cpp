/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <LibMedia/BitReader.h>
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

static constexpr u8 OBU_SEQUENCE_HEADER = 1;
static constexpr u8 SELECT_SCREEN_CONTENT_TOOLS = 2;

// https://aomediacodec.github.io/av1-spec/#color-config-syntax
// Reads the sequence header up to and including color_config()'s color description, which is the only part of it
// that the configuration record does not already carry.
static Optional<CodingIndependentCodePoints> parse_sequence_header_color_description(ReadonlyBytes sequence_header)
{
    BitReader reader { sequence_header };
    auto seq_profile = reader.read_bits<u8>(3);
    reader.skip_bits(1); // still_picture
    auto reduced_still_picture_header = reader.read_bit();

    if (reduced_still_picture_header) {
        reader.skip_bits(5); // seq_level_idx[0]
    } else {
        auto decoder_model_info_present = false;
        u8 buffer_delay_length = 0;
        if (reader.read_bit()) {          // timing_info_present_flag
            reader.skip_bits(32);         // num_units_in_display_tick
            reader.skip_bits(32);         // time_scale
            if (reader.read_bit())        // equal_picture_interval
                reader.read_exp_golomb(); // num_ticks_per_picture_minus_1

            decoder_model_info_present = reader.read_bit();
            if (decoder_model_info_present) {
                buffer_delay_length = reader.read_bits<u8>(5) + 1;
                reader.skip_bits(32); // num_units_in_decoding_tick
                reader.skip_bits(5);  // buffer_removal_time_length_minus_1
                reader.skip_bits(5);  // frame_presentation_time_length_minus_1
            }
        }

        auto initial_display_delay_present = reader.read_bit();
        auto operating_point_count = reader.read_bits<u8>(5) + 1;
        for (int index = 0; index < operating_point_count; index++) {
            reader.skip_bits(12);            // operating_point_idc
            if (reader.read_bits<u8>(5) > 7) // seq_level_idx
                reader.skip_bits(1);         // seq_tier

            if (decoder_model_info_present && reader.read_bit()) { // decoder_model_present_for_this_op
                reader.skip_bits(buffer_delay_length);             // decoder_buffer_delay
                reader.skip_bits(buffer_delay_length);             // encoder_buffer_delay
                reader.skip_bits(1);                               // low_delay_mode_flag
            }
            if (initial_display_delay_present && reader.read_bit()) // initial_display_delay_present_for_this_op
                reader.skip_bits(4);                                // initial_display_delay_minus_1

            if (reader.has_overrun())
                return {};
        }
    }

    auto frame_width_bits = reader.read_bits<u8>(4) + 1;
    auto frame_height_bits = reader.read_bits<u8>(4) + 1;
    reader.skip_bits(frame_width_bits);  // max_frame_width_minus_1
    reader.skip_bits(frame_height_bits); // max_frame_height_minus_1

    if (!reduced_still_picture_header && reader.read_bit()) // frame_id_numbers_present_flag
        reader.skip_bits(4 + 3);                            // delta_frame_id_length_minus_2, additional_frame_id_length_minus_1

    reader.skip_bits(3); // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter

    if (!reduced_still_picture_header) {
        reader.skip_bits(4); // enable_interintra_compound through enable_dual_filter
        auto enable_order_hint = reader.read_bit();
        if (enable_order_hint)
            reader.skip_bits(2); // enable_jnt_comp, enable_ref_frame_mvs

        u8 force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
        if (!reader.read_bit()) // seq_choose_screen_content_tools
            force_screen_content_tools = reader.read_bits<u8>(1);
        if (force_screen_content_tools > 0 && !reader.read_bit()) // seq_choose_integer_mv
            reader.skip_bits(1);                                  // seq_force_integer_mv

        if (enable_order_hint)
            reader.skip_bits(3); // order_hint_bits_minus_1
    }

    reader.skip_bits(3); // enable_superres, enable_cdef, enable_restoration

    auto high_bitdepth = reader.read_bit();
    if (seq_profile == 2 && high_bitdepth)
        reader.skip_bits(1); // twelve_bit

    auto mono_chrome = false;
    if (seq_profile != 1)
        mono_chrome = reader.read_bit();

    auto color_primaries = ColorPrimaries::Unspecified;
    auto transfer_characteristics = TransferCharacteristics::Unspecified;
    auto matrix_coefficients = MatrixCoefficients::Unspecified;
    if (reader.read_bit()) { // color_description_present_flag
        color_primaries = static_cast<ColorPrimaries>(reader.read_bits<u8>(8));
        transfer_characteristics = static_cast<TransferCharacteristics>(reader.read_bits<u8>(8));
        matrix_coefficients = static_cast<MatrixCoefficients>(reader.read_bits<u8>(8));
    }

    auto is_srgb = color_primaries == ColorPrimaries::BT709
        && transfer_characteristics == TransferCharacteristics::SRGB
        && matrix_coefficients == MatrixCoefficients::Identity;

    auto video_full_range_flag = VideoFullRangeFlag::Full;
    if (!is_srgb || mono_chrome)
        video_full_range_flag = reader.read_bit() ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio;

    if (reader.has_overrun())
        return {};

    return CodingIndependentCodePoints { color_primaries, transfer_characteristics, matrix_coefficients, video_full_range_flag };
}

// https://aomediacodec.github.io/av1-isobmff/#av1codecconfigurationbox-syntax
static Optional<CodingIndependentCodePoints> parse_color_description_from_configuration_obus(ReadonlyBytes configuration_obus)
{
    auto remaining = configuration_obus;
    while (!remaining.is_empty()) {
        BitReader reader { remaining };
        if (reader.read_bit()) // obu_forbidden_bit
            return {};
        auto obu_type = reader.read_bits<u8>(4);
        auto has_extension = reader.read_bit();
        auto has_size_field = reader.read_bit();
        reader.skip_bits(1); // obu_reserved_1bit
        if (has_extension)
            reader.skip_bits(8); // temporal_id, spatial_id and reserved bits

        auto payload_size = has_size_field ? reader.read_leb128() : 0;
        if (reader.has_overrun())
            return {};

        auto header_size = reader.bit_position() / 8;
        auto payload = remaining.slice(header_size);
        if (has_size_field) {
            if (payload_size > payload.size())
                return {};
            payload = payload.trim(payload_size);
        }

        if (obu_type == OBU_SEQUENCE_HEADER)
            return parse_sequence_header_color_description(payload);

        remaining = remaining.slice(header_size + payload.size());
    }
    return {};
}

static u8 bit_depth_from_configuration_record(u8 profile, bool high_bitdepth, bool twelve_bit)
{
    if (profile == 2 && high_bitdepth)
        return twelve_bit ? 12 : 10;
    return high_bitdepth ? 10 : 8;
}

// https://aomediacodec.github.io/av1-isobmff/#av1codecconfigurationbox-syntax
Optional<AV1::Parameters> AV1::parse_configuration_record(ReadonlyBytes record)
{
    BitReader reader { record };
    if (!reader.read_bit()) // marker
        return {};
    if (reader.read_bits<u8>(7) != 1) // version
        return {};

    Parameters parameters {};
    parameters.profile = reader.read_bits<u8>(3);
    parameters.level = reader.read_bits<u8>(5);
    parameters.tier = reader.read_bit() ? Tier::High : Tier::Main;
    auto high_bitdepth = reader.read_bit();
    auto twelve_bit = reader.read_bit();

    auto& optional_fields = parameters.optional_fields;
    optional_fields.monochrome = reader.read_bit();
    auto chroma_subsampling_x = reader.read_bit();
    auto chroma_subsampling_y = reader.read_bit();
    optional_fields.subsampling = Subsampling { chroma_subsampling_x, chroma_subsampling_y };
    optional_fields.chroma_sample_position = reader.read_bits<u8>(2);
    reader.skip_bits(3); // Reserved
    reader.skip_bits(5); // initial_presentation_delay_present and the delay or reserved bits
    if (reader.has_overrun())
        return {};

    parameters.bit_depth = bit_depth_from_configuration_record(parameters.profile, high_bitdepth, twelve_bit);

    auto configuration_obus = record.slice(reader.bit_position() / 8);
    optional_fields.cicp = parse_color_description_from_configuration_obus(configuration_obus).value_or({});

    if (!parameters_are_valid(parameters))
        return {};
    return parameters;
}

}
