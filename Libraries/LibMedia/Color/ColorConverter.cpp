/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Matrix4x4.h>
#include <LibMedia/Color/ColorPrimaries.h>
#include <LibMedia/Color/TransferCharacteristics.h>

#include "ColorConverter.h"

namespace Media {

DecoderErrorOr<ColorConverter> ColorConverter::create(u8 bit_depth, CodingIndependentCodePoints input_cicp, CodingIndependentCodePoints output_cicp)
{
    // We'll need to apply tonemapping for linear HDR values.
    bool should_tonemap = false;
    switch (input_cicp.transfer_characteristics()) {
    case TransferCharacteristics::SMPTE2084:
    case TransferCharacteristics::HLG:
        should_tonemap = true;
        break;
    default:
        break;
    }

    // Conversion process:
    // 1. Scale integer YUV values with maximum values of (1 << bit_depth) - 1 into
    //    float 0..1 range.
    //    This can be done with a 3x3 scaling matrix.
    size_t maximum_value = (1u << bit_depth) - 1;
    float scale = 1.0f / maximum_value;
    FloatMatrix4x4 integer_scaling_matrix = {
        scale, 0.0f, 0.0f, 0.0f, // y
        0.0f, scale, 0.0f, 0.0f, // u
        0.0f, 0.0f, scale, 0.0f, // v
        0.0f, 0.0f, 0.0f, 1.0f,  // w
    };

    // 2. Scale YUV values into usable ranges.
    //    For studio range, Y range is 16..235, and UV is 16..240.
    //    UV values should be scaled to a range of -1..1.
    //    This can be done in a 4x4 matrix with translation and scaling.
    float y_min;
    float y_max;
    float uv_min;
    float uv_max;
    if (input_cicp.video_full_range_flag() == VideoFullRangeFlag::Studio) {
        y_min = 16.0f / 255.0f;
        y_max = 235.0f / 255.0f;
        uv_min = y_min;
        uv_max = 240.0f / 255.0f;
    } else {
        y_min = 0.0f;
        y_max = 1.0f;
        uv_min = 0.0f;
        uv_max = 1.0f;
    }
    auto clip_y_scale = 1.0f / (y_max - y_min);
    auto clip_uv_scale = 2.0f / (uv_max - uv_min);

    FloatMatrix4x4 range_scaling_matrix = {
        clip_y_scale, 0.0f, 0.0f, -y_min * clip_y_scale,             // y
        0.0f, clip_uv_scale, 0.0f, -(uv_min * clip_uv_scale + 1.0f), // u
        0.0f, 0.0f, clip_uv_scale, -(uv_min * clip_uv_scale + 1.0f), // v
        0.0f, 0.0f, 0.0f, 1.0f,                                      // w
    };

    // 3. Convert YUV values to RGB.
    //    This is done with coefficients that can be put into a 3x3 matrix
    //    and combined with the above 4x4 matrix to combine steps 1 and 2.
    FloatMatrix4x4 color_conversion_matrix;

    // https://kdashg.github.io/misc/colors/from-coeffs.html
    switch (input_cicp.matrix_coefficients()) {
    case MatrixCoefficients::BT470BG:
    case MatrixCoefficients::BT601:
        color_conversion_matrix = {
            1.0f, 0.0f, 0.70100f, 0.0f,       // y
            1.0f, -0.17207f, -0.35707f, 0.0f, // u
            1.0f, 0.88600f, 0.0f, 0.0f,       // v
            0.0f, 0.0f, 0.0f, 1.0f,           // w
        };
        break;
    case MatrixCoefficients::BT709:
        color_conversion_matrix = {
            1.0f, 0.0f, 0.78740f, 0.0f,       // y
            1.0f, -0.09366f, -0.23406f, 0.0f, // u
            1.0f, 0.92780f, 0.0f, 0.0f,       // v
            0.0f, 0.0f, 0.0f, 1.0f,           // w
        };
        break;
    case MatrixCoefficients::BT2020ConstantLuminance:
    case MatrixCoefficients::BT2020NonConstantLuminance:
        color_conversion_matrix = {
            1.0f, 0.0f, 0.73730f, 0.0f,       // y
            1.0f, -0.08228f, -0.28568f, 0.0f, // u
            1.0f, 0.94070f, 0.0f, 0.0f,       // v
            0.0f, 0.0f, 0.0f, 1.0f,           // w
        };
        break;
    default:
        return DecoderError::format(DecoderErrorCategory::Invalid, "Matrix coefficients {} not supported", matrix_coefficients_to_string(input_cicp.matrix_coefficients()));
    }

    // 4. Apply the inverse transfer function to convert RGB values to the
    //    linear color space.
    //    This will be turned into a lookup table and interpolated to speed
    //    up the conversion.
    auto to_linear_lookup_table = InterpolatedLookupTable<to_linear_size>::create(
        [&](float value) {
            return TransferCharacteristicsConversion::to_linear_luminance(value, input_cicp.transfer_characteristics());
        });

    // 5. Convert the RGB color to CIE XYZ coordinates using the input color
    //    primaries and then to the output color primaries.
    //    This is done with two 3x3 matrices that can be combined into one
    //    matrix multiplication.
    FloatMatrix3x3 color_primaries_matrix = TRY(get_conversion_matrix(input_cicp.color_primaries(), output_cicp.color_primaries()));

    // 6. Apply the output transfer function. For HDR color spaces, this
    //    should apply tonemapping as well.
    //    Use a lookup table as with step 3.
    auto to_non_linear_lookup_table = InterpolatedLookupTable<to_non_linear_size>::create(
        [&](float value) {
            return TransferCharacteristicsConversion::to_non_linear_luminance(value, output_cicp.transfer_characteristics());
        });

    // Expand color primaries matrix with identity elements.
    FloatMatrix4x4 color_primaries_matrix_4x4 = {
        color_primaries_matrix.elements()[0][0],
        color_primaries_matrix.elements()[0][1],
        color_primaries_matrix.elements()[0][2],
        0.0f, // y
        color_primaries_matrix.elements()[1][0],
        color_primaries_matrix.elements()[1][1],
        color_primaries_matrix.elements()[1][2],
        0.0f, // u
        color_primaries_matrix.elements()[2][0],
        color_primaries_matrix.elements()[2][1],
        color_primaries_matrix.elements()[2][2],
        0.0f, // v
        0.0f,
        0.0f,
        0.0f,
        1.0f, // w
    };

    bool should_skip_color_remapping = output_cicp.color_primaries() == input_cicp.color_primaries() && output_cicp.transfer_characteristics() == input_cicp.transfer_characteristics();
    FloatMatrix4x4 input_conversion_matrix = color_conversion_matrix * range_scaling_matrix * integer_scaling_matrix;

    return ColorConverter(bit_depth, input_cicp, should_skip_color_remapping, should_tonemap, input_conversion_matrix, to_linear_lookup_table, color_primaries_matrix_4x4, to_non_linear_lookup_table);
}

}
