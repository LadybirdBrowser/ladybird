/*
 * Copyright (c) 2021, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix.h>
#include <LibGfx/Vector3.h>

namespace Gfx {

template<typename T>
using Matrix3x3 = Matrix<3, T>;

template<typename T>
constexpr static Vector3<T> operator*(Matrix3x3<T> const& m, Vector3<T> const& v)
{
    return Vector3<T>(
        v.x() * m[0, 0] + v.y() * m[0, 1] + v.z() * m[0, 2],
        v.x() * m[1, 0] + v.y() * m[1, 1] + v.z() * m[1, 2],
        v.x() * m[2, 0] + v.y() * m[2, 1] + v.z() * m[2, 2]);
}

typedef Matrix3x3<float> FloatMatrix3x3;
typedef Matrix3x3<double> DoubleMatrix3x3;

}

using Gfx::DoubleMatrix3x3;
using Gfx::FloatMatrix3x3;
using Gfx::Matrix3x3;
