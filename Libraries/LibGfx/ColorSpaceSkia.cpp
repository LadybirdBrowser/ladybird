/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ColorSpace.h>

#include <core/SkColorSpace.h>

namespace Gfx {

ErrorOr<ColorSpace> ColorSpace::load_from_icc_bytes(ReadonlyBytes icc_bytes)
{
    if (icc_bytes.is_empty())
        return ColorSpace {};

    skcms_ICCProfile icc_profile {};
    if (!skcms_Parse(icc_bytes.data(), icc_bytes.size(), &icc_profile))
        return Error::from_string_literal("Failed to parse the ICC profile");

    auto color_space = SkColorSpace::Make(icc_profile);
    if (!color_space && icc_profile.has_trc && icc_profile.has_toXYZD50) {
        skcms_TransferFunction transfer_function;
        float max_error;
        if (skcms_ApproximateCurve(&icc_profile.trc[0], &transfer_function, &max_error))
            color_space = SkColorSpace::MakeRGB(transfer_function, icc_profile.toXYZD50);
    }
    if (!color_space)
        return ColorSpace {};

    // A Skia color space is exactly a transfer function and a matrix to XYZ D50, so this keeps all of it.
    skcms_TransferFunction transfer_function;
    color_space->transferFn(&transfer_function);
    skcms_Matrix3x3 to_xyz_d50;
    VERIFY(color_space->toXYZD50(&to_xyz_d50));

    Parametric parametric;
    parametric.transfer_function = { transfer_function.g, transfer_function.a, transfer_function.b, transfer_function.c, transfer_function.d, transfer_function.e, transfer_function.f };
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column)
            parametric.to_xyz_d50[row * 3 + column] = to_xyz_d50.vals[row][column];
    }
    return ColorSpace { move(parametric) };
}

}
