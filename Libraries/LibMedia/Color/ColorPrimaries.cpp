/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Vector2.h>
#include <LibGfx/Vector3.h>

#include "ColorPrimaries.h"

namespace Media {

ALWAYS_INLINE constexpr FloatVector3 primaries_to_xyz(FloatVector2 primaries)
{
    // https://en.wikipedia.org/wiki/CIE_1931_color_space#CIE_xy_chromaticity_diagram_and_the_CIE_xyY_color_space
    // Luminosity is set to 1.0, so the equations are simplified.
    auto const x = primaries.x();
    auto const y = primaries.y();
    return {
        x / y,
        1.0f,
        (1.0f - x - y) / y
    };
}

ALWAYS_INLINE constexpr FloatMatrix3x3 vectors_to_matrix(FloatVector3 a, FloatVector3 b, FloatVector3 c)
{
    return FloatMatrix3x3(
        a.x(), a.y(), a.z(),
        b.x(), b.y(), b.z(),
        c.x(), c.y(), c.z());
}

ALWAYS_INLINE constexpr FloatMatrix3x3 primaries_matrix(FloatVector2 red, FloatVector2 green, FloatVector2 blue)
{
    return vectors_to_matrix(primaries_to_xyz(red), primaries_to_xyz(green), primaries_to_xyz(blue)).transpose();
}

ALWAYS_INLINE constexpr FloatVector3 matrix_row(FloatMatrix3x3 matrix, size_t row)
{
    return { matrix.elements()[row][0], matrix.elements()[row][1], matrix.elements()[row][2] };
}

ALWAYS_INLINE constexpr FloatMatrix3x3 generate_rgb_to_xyz_matrix(FloatVector2 red_xy, FloatVector2 green_xy, FloatVector2 blue_xy, FloatVector2 white_xy)
{
    // http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
    FloatMatrix3x3 const matrix = primaries_matrix(red_xy, green_xy, blue_xy);
    FloatVector3 const scale_vector = matrix.inverse() * primaries_to_xyz(white_xy);
    return vectors_to_matrix(matrix_row(matrix, 0) * scale_vector, matrix_row(matrix, 1) * scale_vector, matrix_row(matrix, 2) * scale_vector);
}

constexpr FloatVector2 ILLUMINANT_D65 = { 0.3127f, 0.3290f };

constexpr FloatVector2 BT_709_RED = { 0.64f, 0.33f };
constexpr FloatVector2 BT_709_GREEN = { 0.30f, 0.60f };
constexpr FloatVector2 BT_709_BLUE = { 0.15f, 0.06f };

constexpr FloatVector2 BT_2020_RED = { 0.708f, 0.292f };
constexpr FloatVector2 BT_2020_GREEN = { 0.170f, 0.797f };
constexpr FloatVector2 BT_2020_BLUE = { 0.131f, 0.046f };

constexpr FloatMatrix3x3 bt_2020_rgb_to_xyz = generate_rgb_to_xyz_matrix(BT_2020_RED, BT_2020_GREEN, BT_2020_BLUE, ILLUMINANT_D65);
constexpr FloatMatrix3x3 bt_709_rgb_to_xyz = generate_rgb_to_xyz_matrix(BT_709_RED, BT_709_GREEN, BT_709_BLUE, ILLUMINANT_D65);

DecoderErrorOr<FloatMatrix3x3> get_conversion_matrix(ColorPrimaries input_primaries, ColorPrimaries output_primaries)
{
    FloatMatrix3x3 input_conversion_matrix;
    switch (input_primaries) {
    case ColorPrimaries::BT709:
        input_conversion_matrix = bt_709_rgb_to_xyz;
        break;
    case ColorPrimaries::BT2020:
        input_conversion_matrix = bt_2020_rgb_to_xyz;
        break;
    default:
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Conversion of primaries {} is not implemented", color_primaries_to_string(input_primaries));
    }

    FloatMatrix3x3 output_conversion_matrix;
    switch (output_primaries) {
    case ColorPrimaries::BT709:
        output_conversion_matrix = bt_709_rgb_to_xyz.inverse();
        break;
    case ColorPrimaries::BT2020:
        output_conversion_matrix = bt_2020_rgb_to_xyz.inverse();
        break;
    default:
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Conversion of primaries {} is not implemented", color_primaries_to_string(output_primaries));
    }

    return output_conversion_matrix * input_conversion_matrix;
}

}
