/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <LibGfx/Forward.h>

namespace Gfx {

class AffineTransform {
public:
    AffineTransform()
        : m_values { 1, 0, 0, 1, 0, 0 }
    {
    }

    AffineTransform(float a, float b, float c, float d, float e, float f)
        : m_values { a, b, c, d, e, f }
    {
    }

    [[nodiscard]] bool is_identity() const
    {
        return m_values[0] == 1 && m_values[1] == 0 && m_values[2] == 0 && m_values[3] == 1 && m_values[4] == 0 && m_values[5] == 0;
    }

    [[nodiscard]] bool is_identity_or_translation() const
    {
        return m_values[0] == 1 && m_values[1] == 0 && m_values[2] == 0 && m_values[3] == 1;
    }

    [[nodiscard]] bool is_identity_or_translation_or_scale() const
    {
        return m_values[1] == 0 && m_values[2] == 0;
    }

    void map(float unmapped_x, float unmapped_y, float& mapped_x, float& mapped_y) const;

    template<Arithmetic T>
    Point<T> map(Point<T>) const;

    template<Arithmetic T>
    Size<T> map(Size<T>) const;

    template<Arithmetic T>
    Rect<T> map(Rect<T> const&) const;

    Quad<float> map_to_quad(Rect<float> const&) const;

    [[nodiscard]] ALWAYS_INLINE float a() const { return m_values[0]; }
    [[nodiscard]] ALWAYS_INLINE float b() const { return m_values[1]; }
    [[nodiscard]] ALWAYS_INLINE float c() const { return m_values[2]; }
    [[nodiscard]] ALWAYS_INLINE float d() const { return m_values[3]; }
    [[nodiscard]] ALWAYS_INLINE float e() const { return m_values[4]; }
    [[nodiscard]] ALWAYS_INLINE float f() const { return m_values[5]; }

    [[nodiscard]] float x_scale() const;
    [[nodiscard]] float y_scale() const;
    [[nodiscard]] FloatPoint scale() const;
    [[nodiscard]] float x_translation() const;
    [[nodiscard]] float y_translation() const;
    [[nodiscard]] FloatPoint translation() const;
    [[nodiscard]] float rotation() const;

    AffineTransform& scale(float sx, float sy);
    AffineTransform& scale(FloatPoint s);
    AffineTransform& set_scale(float sx, float sy);
    AffineTransform& set_scale(FloatPoint s);
    AffineTransform& translate(float tx, float ty);
    AffineTransform& translate(FloatPoint t);
    AffineTransform& set_translation(float tx, float ty);
    AffineTransform& set_translation(FloatPoint t);
    AffineTransform& rotate_radians(float);
    AffineTransform& skew_radians(float x_radians, float y_radians);
    AffineTransform& multiply(AffineTransform const&);

    float determinant() const;
    Optional<AffineTransform> inverse() const;

private:
    float m_values[6] { 0 };
};

}

template<>
struct AK::Formatter<Gfx::AffineTransform> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::AffineTransform const& value)
    {
        return Formatter<FormatString>::format(builder, "[{} {} {} {} {} {}]"sv, value.a(), value.b(), value.c(), value.d(), value.e(), value.f());
    }
};
