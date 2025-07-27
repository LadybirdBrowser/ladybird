/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Matrix.h>
#include <LibGfx/Vector3.h>
#include <LibGfx/Vector4.h>

namespace Gfx {

template<typename T>
using Matrix4x4 = Matrix<4, T>;

template<typename T>
constexpr static Vector4<T> operator*(Matrix4x4<T> const& m, Vector4<T> const& v)
{
    return Vector4<T>(
        v.x() * m[0, 0] + v.y() * m[0, 1] + v.z() * m[0, 2] + v.w() * m[0, 3],
        v.x() * m[1, 0] + v.y() * m[1, 1] + v.z() * m[1, 2] + v.w() * m[1, 3],
        v.x() * m[2, 0] + v.y() * m[2, 1] + v.z() * m[2, 2] + v.w() * m[2, 3],
        v.x() * m[3, 0] + v.y() * m[3, 1] + v.z() * m[3, 2] + v.w() * m[3, 3]);
}

// FIXME: this is a specific Matrix4x4 * Vector3 interaction that implies W=1; maybe move this out of LibGfx
//        or replace a Matrix4x4 * Vector4 operation?
template<typename T>
constexpr static Vector3<T> transform_point(Matrix4x4<T> const& m, Vector3<T> const& p)
{
    return Vector3<T>(
        p.x() * m[0, 0] + p.y() * m[0, 1] + p.z() * m[0, 2] + m[0, 3],
        p.x() * m[1, 0] + p.y() * m[1, 1] + p.z() * m[1, 2] + m[1, 3],
        p.x() * m[2, 0] + p.y() * m[2, 1] + p.z() * m[2, 2] + m[2, 3]);
}

template<typename T>
constexpr static Matrix4x4<T> translation_matrix(Vector3<T> const& p)
{
    return Matrix4x4<T>(
        1, 0, 0, p.x(),
        0, 1, 0, p.y(),
        0, 0, 1, p.z(),
        0, 0, 0, 1);
}

template<typename T>
constexpr static Matrix4x4<T> scale_matrix(Vector3<T> const& s)
{
    return Matrix4x4<T>(
        s.x(), 0, 0, 0,
        0, s.y(), 0, 0,
        0, 0, s.z(), 0,
        0, 0, 0, 1);
}

template<typename T>
constexpr static Matrix4x4<T> rotation_matrix(Vector3<T> const& axis, T angle)
{
    T c, s;
    AK::sincos(angle, s, c);
    T t = 1 - c;
    T x = axis.x();
    T y = axis.y();
    T z = axis.z();

    return Matrix4x4<T>(
        t * x * x + c, t * x * y - z * s, t * x * z + y * s, 0,
        t * x * y + z * s, t * y * y + c, t * y * z - x * s, 0,
        t * x * z - y * s, t * y * z + x * s, t * z * z + c, 0,
        0, 0, 0, 1);
}

template<typename T>
Gfx::AffineTransform extract_2d_affine_transform(Matrix4x4<T> const& m)
{
    return Gfx::AffineTransform(m[0, 0], m[1, 0], m[0, 1], m[1, 1], m[0, 3], m[1, 3]);
}

typedef Matrix4x4<float> FloatMatrix4x4;
typedef Matrix4x4<double> DoubleMatrix4x4;

}

using Gfx::DoubleMatrix4x4;
using Gfx::FloatMatrix4x4;
using Gfx::Matrix4x4;
