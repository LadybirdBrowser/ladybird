/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Quad.h>
#include <LibGfx/Rect.h>

namespace Gfx {

float AffineTransform::x_scale() const
{
    return AK::hypot(m_values[0], m_values[1]);
}

float AffineTransform::y_scale() const
{
    return AK::hypot(m_values[2], m_values[3]);
}

FloatPoint AffineTransform::scale() const
{
    return { x_scale(), y_scale() };
}

float AffineTransform::x_translation() const
{
    return e();
}

float AffineTransform::y_translation() const
{
    return f();
}

FloatPoint AffineTransform::translation() const
{
    return { x_translation(), y_translation() };
}

AffineTransform& AffineTransform::scale(float sx, float sy)
{
    m_values[0] *= sx;
    m_values[1] *= sx;
    m_values[2] *= sy;
    m_values[3] *= sy;
    return *this;
}

AffineTransform& AffineTransform::scale(FloatPoint s)
{
    return scale(s.x(), s.y());
}

AffineTransform& AffineTransform::set_scale(float sx, float sy)
{
    m_values[0] = sx;
    m_values[1] = 0;
    m_values[2] = 0;
    m_values[3] = sy;
    return *this;
}

AffineTransform& AffineTransform::set_scale(FloatPoint s)
{
    return set_scale(s.x(), s.y());
}

AffineTransform& AffineTransform::skew_radians(float x_radians, float y_radians)
{
    AffineTransform skew_transform(1, AK::tan(y_radians), AK::tan(x_radians), 1, 0, 0);
    multiply(skew_transform);
    return *this;
}

AffineTransform& AffineTransform::translate(float tx, float ty)
{
    if (is_identity_or_translation()) {
        m_values[4] += tx;
        m_values[5] += ty;
        return *this;
    }
    m_values[4] += tx * m_values[0] + ty * m_values[2];
    m_values[5] += tx * m_values[1] + ty * m_values[3];
    return *this;
}

AffineTransform& AffineTransform::translate(FloatPoint t)
{
    return translate(t.x(), t.y());
}

AffineTransform& AffineTransform::set_translation(float tx, float ty)
{
    m_values[4] = tx;
    m_values[5] = ty;
    return *this;
}

AffineTransform& AffineTransform::set_translation(FloatPoint t)
{
    return set_translation(t.x(), t.y());
}

AffineTransform& AffineTransform::multiply(AffineTransform const& other)
{
    if (other.is_identity())
        return *this;
    AffineTransform result;
    result.m_values[0] = other.a() * a() + other.b() * c();
    result.m_values[1] = other.a() * b() + other.b() * d();
    result.m_values[2] = other.c() * a() + other.d() * c();
    result.m_values[3] = other.c() * b() + other.d() * d();
    result.m_values[4] = other.e() * a() + other.f() * c() + e();
    result.m_values[5] = other.e() * b() + other.f() * d() + f();
    *this = result;
    return *this;
}

AffineTransform& AffineTransform::rotate_radians(float radians)
{
    float sin_angle;
    float cos_angle;
    AK::sincos(radians, sin_angle, cos_angle);
    AffineTransform rotation(cos_angle, sin_angle, -sin_angle, cos_angle, 0, 0);
    multiply(rotation);
    return *this;
}

float AffineTransform::determinant() const
{
    return a() * d() - b() * c();
}

Optional<AffineTransform> AffineTransform::inverse() const
{
    auto det = determinant();
    if (det == 0)
        return {};
    return AffineTransform {
        d() / det,
        -b() / det,
        -c() / det,
        a() / det,
        (c() * f() - d() * e()) / det,
        (b() * e() - a() * f()) / det,
    };
}

void AffineTransform::map(float unmapped_x, float unmapped_y, float& mapped_x, float& mapped_y) const
{
    mapped_x = a() * unmapped_x + c() * unmapped_y + e();
    mapped_y = b() * unmapped_x + d() * unmapped_y + f();
}

template<>
IntPoint AffineTransform::map(IntPoint point) const
{
    float mapped_x;
    float mapped_y;
    map(static_cast<float>(point.x()), static_cast<float>(point.y()), mapped_x, mapped_y);
    return { round_to<int>(mapped_x), round_to<int>(mapped_y) };
}

template<>
FloatPoint AffineTransform::map(FloatPoint point) const
{
    float mapped_x;
    float mapped_y;
    map(point.x(), point.y(), mapped_x, mapped_y);
    return { mapped_x, mapped_y };
}

template<>
IntSize AffineTransform::map(IntSize size) const
{
    return {
        round_to<int>(static_cast<float>(size.width()) * x_scale()),
        round_to<int>(static_cast<float>(size.height()) * y_scale()),
    };
}

template<>
FloatSize AffineTransform::map(FloatSize size) const
{
    return { size.width() * x_scale(), size.height() * y_scale() };
}

template<typename T>
static T smallest_of(T p1, T p2, T p3, T p4)
{
    return min(min(p1, p2), min(p3, p4));
}

template<typename T>
static T largest_of(T p1, T p2, T p3, T p4)
{
    return max(max(p1, p2), max(p3, p4));
}

template<>
FloatRect AffineTransform::map(FloatRect const& rect) const
{
    if (is_identity()) {
        return rect;
    }
    if (is_identity_or_translation()) {
        return rect.translated(e(), f());
    }
    FloatPoint p1 = map(rect.top_left());
    FloatPoint p2 = map(rect.top_right());
    FloatPoint p3 = map(rect.bottom_right());
    FloatPoint p4 = map(rect.bottom_left());
    float left = smallest_of(p1.x(), p2.x(), p3.x(), p4.x());
    float top = smallest_of(p1.y(), p2.y(), p3.y(), p4.y());
    float right = largest_of(p1.x(), p2.x(), p3.x(), p4.x());
    float bottom = largest_of(p1.y(), p2.y(), p3.y(), p4.y());
    return { left, top, right - left, bottom - top };
}

template<>
IntRect AffineTransform::map(IntRect const& rect) const
{
    return enclosing_int_rect(map(FloatRect(rect)));
}

Quad<float> AffineTransform::map_to_quad(Rect<float> const& rect) const
{
    return {
        map(rect.top_left()),
        map(rect.top_right()),
        map(rect.bottom_right()),
        map(rect.bottom_left()),
    };
}

float AffineTransform::rotation() const
{
    auto rotation = AK::atan2(b(), a());
    while (rotation < -AK::Pi<float>)
        rotation += 2.0f * AK::Pi<float>;
    while (rotation > AK::Pi<float>)
        rotation -= 2.0f * AK::Pi<float>;
    return rotation;
}

}
