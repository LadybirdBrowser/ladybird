/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibMediaCodecs/BitReader.h>
#include <LibMediaCodecs/Codecs/DolbyVision.h>

// Dolby does not publish the RPU syntax, so this follows FFmpeg's parser in libavcodec/dovi_rpudec.c. Only the fields
// that precede the level 1 metadata are read, and everything else is skipped.

namespace Media::Codecs {

static constexpr u8 RPU_PREFIX = 0x19;
static constexpr u8 RPU_TYPE_WITH_DISPLAY_MANAGEMENT = 2;
static constexpr u16 RPU_FORMAT_PROFILE_MASK = 0x700;
static constexpr u8 FIXED_POINT_COEFFICIENTS = 0;
static constexpr u8 FLOATING_POINT_COEFFICIENTS = 1;
static constexpr u64 MINIMUM_FIXED_POINT_FRACTION_BITS = 13;
static constexpr u64 MAXIMUM_FIXED_POINT_FRACTION_BITS = 32;
static constexpr u64 MAXIMUM_BIT_DEPTH_MINUS_8 = 8;
static constexpr u64 MAXIMUM_PIVOT_COUNT_MINUS_2 = 7;
static constexpr u64 POLYNOMIAL_MAPPING = 0;
static constexpr u64 MULTIVARIATE_MULTIPLE_REGRESSION_MAPPING = 1;
static constexpr u64 MAXIMUM_POLYNOMIAL_ORDER_MINUS_1 = 1;
static constexpr u8 MAXIMUM_MULTIVARIATE_MULTIPLE_REGRESSION_ORDER_MINUS_1 = 2;
static constexpr size_t MULTIVARIATE_MULTIPLE_REGRESSION_COEFFICIENTS_PER_ORDER = 7;
static constexpr u8 LINEAR_DEAD_ZONE_QUANTIZATION = 0;

// The colour transform of the display management metadata: the YCbCr to RGB matrix and offsets, the RGB to LMS matrix,
// the signal's transfer, bit depth, colour space, chroma format and range, and the source display's luminance range
// and diagonal.
static constexpr size_t DISPLAY_MANAGEMENT_COLOR_TRANSFORM_BIT_COUNT = 9 * 16 + 3 * 32 + 9 * 16 + 3 * 16 + 32 + 5 + 2 + 2 + 2 + 12 + 12 + 10;
static constexpr u8 LEVEL_1_EXTENSION_BLOCK = 1;
static constexpr size_t PERCEPTUAL_QUANTIZER_CODE_BIT_COUNT = 12;

static float luminance_of_perceptual_quantizer_code(u16 code)
{
    // SMPTE ST 2084's EOTF.
    constexpr double m1 = 2610.0 / 16384;
    constexpr double m2 = 2523.0 / 4096 * 128;
    constexpr double c1 = 3424.0 / 4096;
    constexpr double c2 = 2413.0 / 4096 * 32;
    constexpr double c3 = 2392.0 / 4096 * 32;
    constexpr double peak_luminance = 10000;
    auto signal = static_cast<double>(code) / ((1 << PERCEPTUAL_QUANTIZER_CODE_BIT_COUNT) - 1);
    auto signal_power = AK::pow(signal, 1 / m2);
    auto linear = AK::pow(max(signal_power - c1, 0.0) / (c2 - c3 * signal_power), 1 / m1);
    return static_cast<float>(linear * peak_luminance);
}

Optional<float> dolby_vision_maximum_scene_luminance(ReadonlyBytes rpu)
{
    if (rpu.is_empty() || rpu[0] != RPU_PREFIX)
        return {};

    BitReader reader { rpu.slice(1) };
    if (reader.read_bits<u8>(6) != RPU_TYPE_WITH_DISPLAY_MANAGEMENT)
        return {};
    auto rpu_format = reader.read_bits<u16>(11);
    reader.skip_bits(4 + 4); // vdr_rpu_profile, vdr_rpu_level

    if (!reader.read_bit()) // vdr_seq_info_present_flag
        return {};
    reader.skip_bits(1); // chroma_resampling_explicit_filter_flag
    auto coefficient_data_type = reader.read_bits<u8>(2);
    u64 coefficient_fraction_bit_count = 0;
    if (coefficient_data_type == FIXED_POINT_COEFFICIENTS) {
        coefficient_fraction_bit_count = reader.read_exp_golomb();
        if (coefficient_fraction_bit_count < MINIMUM_FIXED_POINT_FRACTION_BITS || coefficient_fraction_bit_count > MAXIMUM_FIXED_POINT_FRACTION_BITS)
            return {};
    } else if (coefficient_data_type != FLOATING_POINT_COEFFICIENTS) {
        return {};
    }
    reader.skip_bits(2 + 1); // vdr_rpu_normalized_idc, bl_video_full_range_flag

    if ((rpu_format & RPU_FORMAT_PROFILE_MASK) != 0)
        return {};
    auto base_layer_bit_depth_minus_8 = reader.read_exp_golomb();
    // The upper bits of the enhancement layer's bit depth hold the ext_mapping_idc.
    auto enhancement_layer_bit_depth_minus_8 = reader.read_exp_golomb() & 0xFF;
    reader.read_exp_golomb(); // vdr_bit_depth_minus8
    if (base_layer_bit_depth_minus_8 > MAXIMUM_BIT_DEPTH_MINUS_8 || enhancement_layer_bit_depth_minus_8 > MAXIMUM_BIT_DEPTH_MINUS_8)
        return {};
    auto base_layer_bit_depth = base_layer_bit_depth_minus_8 + 8;
    auto enhancement_layer_bit_depth = enhancement_layer_bit_depth_minus_8 + 8;
    reader.skip_bits(1); // spatial_resampling_filter_flag
    auto display_management_compression = reader.read_bits<u8>(3);
    reader.skip_bits(1);                                   // el_spatial_resampling_filter_flag
    auto uses_nonlinear_quantization = !reader.read_bit(); // disable_residual_flag

    auto display_management_metadata_is_present = reader.read_bit();
    if (display_management_compression > 1)
        return {};

    auto skip_coefficient = [&] {
        if (coefficient_data_type == FIXED_POINT_COEFFICIENTS) {
            reader.read_exp_golomb();
            reader.skip_bits(coefficient_fraction_bit_count);
        } else {
            reader.skip_bits(32);
        }
    };

    if (reader.read_bit()) {      // use_prev_vdr_rpu_flag
        reader.read_exp_golomb(); // prev_vdr_rpu_id
    } else {
        reader.read_exp_golomb(); // vdr_rpu_id
        reader.read_exp_golomb(); // mapping_color_space
        reader.read_exp_golomb(); // mapping_chroma_format_idc

        Array<u64, 3> pivot_counts {};
        for (auto& pivot_count : pivot_counts) {
            auto pivot_count_minus_2 = reader.read_exp_golomb();
            if (pivot_count_minus_2 > MAXIMUM_PIVOT_COUNT_MINUS_2)
                return {};
            pivot_count = pivot_count_minus_2 + 2;
            reader.skip_bits(pivot_count * base_layer_bit_depth);
        }

        u8 nonlinear_quantization_method = 0;
        if (uses_nonlinear_quantization) {
            nonlinear_quantization_method = reader.read_bits<u8>(3);
            if (nonlinear_quantization_method > LINEAR_DEAD_ZONE_QUANTIZATION)
                return {};
            reader.skip_bits(2 * base_layer_bit_depth); // nlq_pred_pivot_value
        }
        reader.read_exp_golomb(); // num_x_partitions_minus1
        reader.read_exp_golomb(); // num_y_partitions_minus1

        for (auto pivot_count : pivot_counts) {
            for (u64 piece = 0; piece + 1 < pivot_count; piece++) {
                auto mapping = reader.read_exp_golomb();
                if (mapping == POLYNOMIAL_MAPPING) {
                    auto order_minus_1 = reader.read_exp_golomb();
                    if (order_minus_1 > MAXIMUM_POLYNOMIAL_ORDER_MINUS_1)
                        return {};
                    if (order_minus_1 == 0 && reader.read_bit()) // linear_interp_flag
                        return {};
                    for (u64 coefficient = 0; coefficient < order_minus_1 + 2; coefficient++)
                        skip_coefficient();
                } else if (mapping == MULTIVARIATE_MULTIPLE_REGRESSION_MAPPING) {
                    auto order_minus_1 = reader.read_bits<u8>(2);
                    if (order_minus_1 > MAXIMUM_MULTIVARIATE_MULTIPLE_REGRESSION_ORDER_MINUS_1)
                        return {};
                    skip_coefficient(); // mmr_constant
                    for (size_t coefficient = 0; coefficient < (order_minus_1 + 1u) * MULTIVARIATE_MULTIPLE_REGRESSION_COEFFICIENTS_PER_ORDER; coefficient++)
                        skip_coefficient();
                } else {
                    return {};
                }
            }
        }

        if (uses_nonlinear_quantization) {
            for (size_t component = 0; component < 3; component++) {
                reader.skip_bits(enhancement_layer_bit_depth); // nlq_offset
                skip_coefficient();                            // vdr_in_max
                if (nonlinear_quantization_method == LINEAR_DEAD_ZONE_QUANTIZATION) {
                    skip_coefficient(); // linear_deadzone_slope
                    skip_coefficient(); // linear_deadzone_threshold
                }
            }
        }
    }

    if (!display_management_metadata_is_present)
        return {};
    reader.read_exp_golomb(); // affected_dm_metadata_id
    reader.read_exp_golomb(); // current_dm_metadata_id
    reader.read_exp_golomb(); // scene_refresh_flag
    if (display_management_compression == 0)
        reader.skip_bits(DISPLAY_MANAGEMENT_COLOR_TRANSFORM_BIT_COUNT);

    // The level 1 metadata is among the content mapping version 2.9 extension blocks, which come first.
    auto block_count = reader.read_exp_golomb();
    reader.align_to_byte();
    for (u64 block = 0; block < block_count && !reader.has_overrun(); block++) {
        auto block_byte_count = reader.read_exp_golomb();
        auto level = reader.read_bits<u8>(8);
        if (level == LEVEL_1_EXTENSION_BLOCK) {
            // min_pq, max_pq and avg_pq
            if (block_byte_count * 8 < 3 * PERCEPTUAL_QUANTIZER_CODE_BIT_COUNT)
                return {};
            reader.skip_bits(PERCEPTUAL_QUANTIZER_CODE_BIT_COUNT); // min_pq
            auto maximum_code = reader.read_bits<u16>(PERCEPTUAL_QUANTIZER_CODE_BIT_COUNT);
            if (reader.has_overrun() || maximum_code == 0)
                return {};
            return luminance_of_perceptual_quantizer_code(maximum_code);
        }
        reader.skip_bits(block_byte_count * 8);
    }
    return {};
}

static constexpr u8 UNITED_STATES_COUNTRY_CODE = 0xB5;
static constexpr u16 DOLBY_TERMINAL_PROVIDER_CODE = 0x003B;
static constexpr u32 DOLBY_VISION_TERMINAL_PROVIDER_ORIENTED_CODE = 0x00000800;
// The fields of an EMDF container header are fixed for Dolby Vision, so they read as one 27-bit value.
static constexpr u32 EMDF_HEADER = 0x01BE6841;
static constexpr size_t EMDF_HEADER_BIT_COUNT = 27;
static constexpr u32 MINIMUM_EMDF_PAYLOAD_SIZE = 6;
static constexpr u32 MAXIMUM_EMDF_PAYLOAD_SIZE = 512;

// EMDF's variable_bits(): a value that grows by another group of bits for as long as a following bit is set. Two
// groups of 8 bits are enough for any payload size up to the maximum, so a third makes the size invalid.
static Optional<u32> read_emdf_payload_size(BitReader& reader)
{
    constexpr size_t group_bit_count = 8;
    u32 value = reader.read_bits<u32>(group_bit_count);
    if (!reader.read_bit())
        return value;
    value = ((value + 1) << group_bit_count) | reader.read_bits<u32>(group_bit_count);
    if (reader.read_bit())
        return {};
    return value;
}

Optional<float> dolby_vision_maximum_scene_luminance_of_t35(ReadonlyBytes t35_message)
{
    BitReader reader { t35_message };
    if (reader.read_bits<u8>(8) != UNITED_STATES_COUNTRY_CODE)
        return {};
    if (reader.read_bits<u16>(16) != DOLBY_TERMINAL_PROVIDER_CODE)
        return {};
    if (reader.read_bits<u32>(32) != DOLBY_VISION_TERMINAL_PROVIDER_ORIENTED_CODE)
        return {};
    if (reader.read_bits<u32>(EMDF_HEADER_BIT_COUNT) != EMDF_HEADER)
        return {};
    auto payload_size = read_emdf_payload_size(reader).value_or(0);
    if (reader.has_overrun() || payload_size < MINIMUM_EMDF_PAYLOAD_SIZE || payload_size > MAXIMUM_EMDF_PAYLOAD_SIZE)
        return {};

    // The payload is off byte alignment by a bit, and the RPU in it lacks the prefix an H.265 NAL unit gives it.
    Array<u8, MAXIMUM_EMDF_PAYLOAD_SIZE + 1> rpu;
    rpu[0] = RPU_PREFIX;
    for (size_t index = 0; index < payload_size; index++)
        rpu[index + 1] = reader.read_bits<u8>(8);
    if (reader.has_overrun())
        return {};
    return dolby_vision_maximum_scene_luminance(ReadonlyBytes { rpu.data(), payload_size + 1 });
}

}
