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
    auto const& elements = m.elements();
    return Vector3<T>(
        v.x() * elements[0][0] + v.y() * elements[0][1] + v.z() * elements[0][2],
        v.x() * elements[1][0] + v.y() * elements[1][1] + v.z() * elements[1][2],
        v.x() * elements[2][0] + v.y() * elements[2][1] + v.z() * elements[2][2]);
}

typedef Matrix3x3<float> FloatMatrix3x3;
typedef Matrix3x3<double> DoubleMatrix3x3;
}

using Gfx::DoubleMatrix3x3;
using Gfx::FloatMatrix3x3;
using Gfx::Matrix3x3;
