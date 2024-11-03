/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Orientation.h>
#include <LibIPC/Forward.h>
#include <math.h>

namespace Gfx {

template<typename T>
class Point {
public:
    Point() = default;

    constexpr Point(T x, T y)
        : m_x(x)
        , m_y(y)
    {
    }

    template<typename U>
    constexpr Point(U x, U y)
        : m_x(x)
        , m_y(y)
    {
    }

    template<typename U>
    explicit Point(Point<U> const& other)
        : m_x(other.x())
        , m_y(other.y())
    {
    }

    [[nodiscard]] constexpr ALWAYS_INLINE T x() const { return m_x; }
    [[nodiscard]] constexpr ALWAYS_INLINE T y() const { return m_y; }

    ALWAYS_INLINE void set_x(T x) { m_x = x; }
    ALWAYS_INLINE void set_y(T y) { m_y = y; }

    [[nodiscard]] ALWAYS_INLINE bool is_zero() const { return m_x == 0 && m_y == 0; }

    void translate_by(T dx, T dy)
    {
        m_x += dx;
        m_y += dy;
    }

    ALWAYS_INLINE void translate_by(T dboth) { translate_by(dboth, dboth); }
    ALWAYS_INLINE void translate_by(Point<T> const& delta) { translate_by(delta.x(), delta.y()); }

    void scale_by(T dx, T dy)
    {
        m_x *= dx;
        m_y *= dy;
    }

    ALWAYS_INLINE void scale_by(T dboth) { scale_by(dboth, dboth); }
    ALWAYS_INLINE void scale_by(Point<T> const& delta) { scale_by(delta.x(), delta.y()); }

    void transform_by(AffineTransform const& transform) { *this = transform.map(*this); }

    [[nodiscard]] Point<T> translated(Point<T> const& delta) const
    {
        Point<T> point = *this;
        point.translate_by(delta);
        return point;
    }

    [[nodiscard]] Point<T> translated(T dx, T dy) const
    {
        Point<T> point = *this;
        point.translate_by(dx, dy);
        return point;
    }

    [[nodiscard]] Point<T> translated(T dboth) const
    {
        Point<T> point = *this;
        point.translate_by(dboth, dboth);
        return point;
    }

    [[nodiscard]] Point<T> scaled(T dboth) const
    {
        Point<T> point = *this;
        point.scale_by(dboth);
        return point;
    }

    [[nodiscard]] Point<T> scaled(Point<T> const& delta) const
    {
        Point<T> point = *this;
        point.scale_by(delta);
        return point;
    }

    [[nodiscard]] Point<T> scaled(T sx, T sy) const
    {
        Point<T> point = *this;
        point.scale_by(sx, sy);
        return point;
    }

    [[nodiscard]] Point<T> transformed(AffineTransform const& transform) const
    {
        Point<T> point = *this;
        point.transform_by(transform);
        return point;
    }

    void constrain(Rect<T> const&);
    [[nodiscard]] Point<T> constrained(Rect<T> const& rect) const
    {
        Point<T> point = *this;
        point.constrain(rect);
        return point;
    }

    [[nodiscard]] Point<T> moved_left(T amount) const { return { x() - amount, y() }; }
    [[nodiscard]] Point<T> moved_right(T amount) const { return { x() + amount, y() }; }
    [[nodiscard]] Point<T> moved_up(T amount) const { return { x(), y() - amount }; }
    [[nodiscard]] Point<T> moved_down(T amount) const { return { x(), y() + amount }; }

    template<class U>
    [[nodiscard]] bool operator==(Point<U> const& other) const
    {
        return x() == other.x() && y() == other.y();
    }

    [[nodiscard]] Point<T> operator+(Point<T> const& other) const { return { m_x + other.m_x, m_y + other.m_y }; }

    Point<T>& operator+=(Point<T> const& other)
    {
        m_x += other.m_x;
        m_y += other.m_y;
        return *this;
    }

    [[nodiscard]] Point<T> operator-() const { return { -m_x, -m_y }; }

    [[nodiscard]] Point<T> operator-(Point<T> const& other) const { return { m_x - other.m_x, m_y - other.m_y }; }

    Point<T>& operator-=(Point<T> const& other)
    {
        m_x -= other.m_x;
        m_y -= other.m_y;
        return *this;
    }

    [[nodiscard]] Point<T> operator*(T factor) const { return { m_x * factor, m_y * factor }; }

    Point<T>& operator*=(T factor)
    {
        m_x *= factor;
        m_y *= factor;
        return *this;
    }

    [[nodiscard]] Point<T> operator/(T factor) const { return { m_x / factor, m_y / factor }; }

    Point<T>& operator/=(T factor)
    {
        m_x /= factor;
        m_y /= factor;
        return *this;
    }

    [[nodiscard]] T primary_offset_for_orientation(Orientation orientation) const
    {
        return orientation == Orientation::Vertical ? y() : x();
    }

    void set_primary_offset_for_orientation(Orientation orientation, T value)
    {
        if (orientation == Orientation::Vertical) {
            set_y(value);
        } else {
            set_x(value);
        }
    }

    [[nodiscard]] T secondary_offset_for_orientation(Orientation orientation) const
    {
        return orientation == Orientation::Vertical ? x() : y();
    }

    void set_secondary_offset_for_orientation(Orientation orientation, T value)
    {
        if (orientation == Orientation::Vertical) {
            set_x(value);
        } else {
            set_y(value);
        }
    }

    [[nodiscard]] T dx_relative_to(Point<T> const& other) const
    {
        return x() - other.x();
    }

    [[nodiscard]] T dy_relative_to(Point<T> const& other) const
    {
        return y() - other.y();
    }

    // Returns pixels moved from other in either direction
    [[nodiscard]] T pixels_moved(Point<T> const& other) const
    {
        return max(AK::abs(dx_relative_to(other)), AK::abs(dy_relative_to(other)));
    }

    [[nodiscard]] float distance_from(Point<T> const& other) const
    {
        if (*this == other)
            return 0;
        return AK::hypot<float>(m_x - other.m_x, m_y - other.m_y);
    }

    [[nodiscard]] Point absolute_relative_distance_to(Point const& other) const
    {
        return { AK::abs(dx_relative_to(other)), AK::abs(dy_relative_to(other)) };
    }

    [[nodiscard]] Point end_point_for_aspect_ratio(Point const& previous_end_point, float aspect_ratio) const;

    template<typename U>
    requires(!IsSame<T, U>)
    [[nodiscard]] Point<U> to_type() const
    {
        return Point<U>(*this);
    }

    template<typename U>
    [[nodiscard]] Point<U> to_rounded() const
    {
        return Point<U>(roundf(x()), roundf(y()));
    }

    template<typename U>
    requires FloatingPoint<T>
    [[nodiscard]] Point<U> to_ceiled() const
    {
        return Point<U>(ceil(x()), ceil(y()));
    }

    template<typename U>
    requires FloatingPoint<T>
    [[nodiscard]] Point<U> to_floored() const
    {
        return Point<U>(AK::floor(x()), AK::floor(y()));
    }

    [[nodiscard]] ByteString to_byte_string() const;

private:
    T m_x { 0 };
    T m_y { 0 };
};
using IntPoint = Point<int>;
using FloatPoint = Point<float>;

template<typename T>
inline Point<T> linear_interpolate(Point<T> const& p1, Point<T> const& p2, float t)
{
    return Point<T> { p1.x() + t * (p2.x() - p1.x()), p1.y() + t * (p2.y() - p1.y()) };
}

template<typename T>
inline Point<T> quadratic_interpolate(Point<T> const& p1, Point<T> const& p2, Point<T> const& c1, float t)
{
    return linear_interpolate(linear_interpolate(p1, c1, t), linear_interpolate(c1, p2, t), t);
}

template<typename T>
inline Point<T> cubic_interpolate(Point<T> const& p1, Point<T> const& p2, Point<T> const& c1, Point<T> const& c2, float t)
{
    return linear_interpolate(quadratic_interpolate(p1, c1, c2, t), quadratic_interpolate(c1, c2, p2, t), t);
}

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Point<T>> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Point<T> const& value)
    {
        return Formatter<FormatString>::format(builder, "[{},{}]"sv, value.x(), value.y());
    }
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::IntPoint const&);
template<>
ErrorOr<void> encode(Encoder&, Gfx::FloatPoint const&);

template<>
ErrorOr<Gfx::IntPoint> decode(Decoder&);
template<>
ErrorOr<Gfx::FloatPoint> decode(Decoder&);

}

template<typename T>
struct AK::Traits<Gfx::Point<T>> : public AK::DefaultTraits<Gfx::Point<T>> {
    static constexpr bool is_trivial() { return false; }
    static unsigned hash(Gfx::Point<T> const& point)
    {
        return pair_int_hash(AK::Traits<T>::hash(point.x()), AK::Traits<T>::hash(point.y()));
    }
};
