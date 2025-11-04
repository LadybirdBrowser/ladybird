/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>

namespace Gfx {

template<typename T>
class Line {
public:
    Line() = default;

    Line(Point<T> a, Point<T> b)
        : m_a(a)
        , m_b(b)
    {
    }

    template<typename U>
    Line(U a, U b)
        : m_a(a)
        , m_b(b)
    {
    }

    template<typename U>
    explicit Line(Line<U> const& other)
        : m_a(other.a())
        , m_b(other.b())
    {
    }

    bool intersects(Line const& other) const
    {
        return intersected(other).has_value();
    }

    Optional<Point<T>> intersected(Line const& other) const
    {
        auto cross_product = [](Point<T> const& p1, Point<T> const& p2) {
            return p1.x() * p2.y() - p1.y() * p2.x();
        };
        auto r = m_b - m_a;
        auto s = other.m_b - other.m_a;
        auto delta_a = other.m_a - m_a;
        auto num = cross_product(delta_a, r);
        auto denom = cross_product(r, s);

        bool parallel = denom == 0;
        bool collinear = num == 0;
        if (parallel) {
            if (collinear)
                return collinear_intersection_point(other);
            return {};
        }

        using Calc = Conditional<IsFloatingPoint<T>, T, double>;

        auto u = static_cast<Calc>(num) / static_cast<Calc>(denom);
        if (u < static_cast<Calc>(0) || u > static_cast<Calc>(1)) {
            // Lines are not parallel and don't intersect
            return {};
        }
        auto t = static_cast<Calc>(cross_product(delta_a, s)) / static_cast<Calc>(denom);
        if (t < static_cast<Calc>(0) || t > static_cast<Calc>(1)) {
            // Lines are not parallel and don't intersect
            return {};
        }
        auto x = static_cast<Calc>(m_a.x()) + t * static_cast<Calc>(r.x());
        auto y = static_cast<Calc>(m_a.y()) + t * static_cast<Calc>(r.y());
        if constexpr (IsFloatingPoint<T>)
            return Point<T> { static_cast<T>(x), static_cast<T>(y) };
        else
            return Point<T> { round_to<T>(x), round_to<T>(y) };
    }

    float length() const
    {
        return m_a.distance_from(m_b);
    }

    Point<T> closest_to(Point<T> const& point) const
    {
        if (m_a == m_b)
            return m_a;
        auto delta_a = point.x() - m_a.x();
        auto delta_b = point.y() - m_a.y();
        auto delta_c = m_b.x() - m_a.x();
        auto delta_d = m_b.y() - m_a.y();
        auto len_sq = delta_c * delta_c + delta_d * delta_d;
        using Calc = Conditional<IsFloatingPoint<T>, T, double>;
        Calc param = static_cast<Calc>(-1);
        if (len_sq != 0)
            param = static_cast<Calc>(delta_a * delta_c + delta_b * delta_d) / static_cast<Calc>(len_sq);
        if (param < static_cast<Calc>(0))
            return m_a;
        if (param > static_cast<Calc>(1))
            return m_b;
        auto rx = static_cast<Calc>(m_a.x()) + param * static_cast<Calc>(delta_c);
        auto ry = static_cast<Calc>(m_a.y()) + param * static_cast<Calc>(delta_d);
        if constexpr (IsFloatingPoint<T>)
            return { static_cast<T>(rx), static_cast<T>(ry) };
        else
            return { round_to<T>(rx), round_to<T>(ry) };
    }

    Line<T> shortest_line_to(Point<T> const& point) const
    {
        return { closest_to(point), point };
    }

    float distance_to(Point<T> const& point) const
    {
        return shortest_line_to(point).length();
    }

    Point<T> const& a() const { return m_a; }
    Point<T> const& b() const { return m_b; }

    Line<T> rotated(float radians)
    {
        Gfx::AffineTransform rotation_transform;
        rotation_transform.rotate_radians(radians);

        Line<T> line = *this;
        line.set_a(line.a().transformed(rotation_transform));
        line.set_b(line.b().transformed(rotation_transform));
        return line;
    }

    void set_a(Point<T> const& a) { m_a = a; }
    void set_b(Point<T> const& b) { m_b = b; }

    Line<T> scaled(T sx, T sy) const
    {
        Line<T> line = *this;
        line.set_a(line.a().scaled(sx, sy));
        line.set_b(line.b().scaled(sx, sy));
        return line;
    }

    Line<T> translated(Point<T> const& delta) const
    {
        Line<T> line = *this;
        line.set_a(line.a().translated(delta));
        line.set_b(line.b().translated(delta));
        return line;
    }

    template<typename U>
    requires(!IsSame<T, U>)
    [[nodiscard]] ALWAYS_INLINE constexpr Line<U> to_type() const
    {
        return Line<U>(*this);
    }

    ByteString to_byte_string() const;

private:
    /*
     * @brief Return a single point representing the intersection of two collinear segments.
     * Compute the midpoint of the overlap in X and Y independently, which maps to the
     * midpoint of the overlapping segment for collinear lines. If there is no overlap,
     * return empty.
     */
    Optional<Point<T>> collinear_intersection_point(Line const& other) const
    {
        // Handle degenerate: A is a point
        if (m_a == m_b) {
            // If A lies on B's span (collinearity is already known), it must also be within B's box
            auto bx0 = min(other.m_a.x(), other.m_b.x());
            auto bx1 = max(other.m_a.x(), other.m_b.x());
            auto by0 = min(other.m_a.y(), other.m_b.y());
            auto by1 = max(other.m_a.y(), other.m_b.y());
            if (m_a.x() < bx0 || m_a.x() > bx1 || m_a.y() < by0 || m_a.y() > by1)
                return {};
            return m_a;
        }

        // Overlap ranges for X and Y
        auto ax0 = min(m_a.x(), m_b.x());
        auto ax1 = max(m_a.x(), m_b.x());
        auto ay0 = min(m_a.y(), m_b.y());
        auto ay1 = max(m_a.y(), m_b.y());

        auto bx0 = min(other.m_a.x(), other.m_b.x());
        auto bx1 = max(other.m_a.x(), other.m_b.x());
        auto by0 = min(other.m_a.y(), other.m_b.y());
        auto by1 = max(other.m_a.y(), other.m_b.y());

        auto ox0 = max(ax0, bx0);
        auto ox1 = min(ax1, bx1);
        auto oy0 = max(ay0, by0);
        auto oy1 = min(ay1, by1);

        if (ox1 < ox0 || oy1 < oy0)
            return {};

        // midpoint helper avoiding overflow for integers: start + (end - start) / 2
        auto midpoint = [](auto const& start, auto const& end) {
            if constexpr (IsFloatingPoint<T>)
                return static_cast<T>((start + end) / static_cast<T>(2));
            else
                return static_cast<T>(start + (end - start) / 2);
        };

        T mx = midpoint(ox0, ox1);
        T my = midpoint(oy0, oy1);
        return Point<T> { mx, my };
    }
    Point<T> m_a;
    Point<T> m_b;
};

template<>
inline ByteString IntLine::to_byte_string() const
{
    return ByteString::formatted("[{},{} -> {},{}]", m_a.x(), m_a.y(), m_b.x(), m_b.y());
}

template<>
inline ByteString FloatLine::to_byte_string() const
{
    return ByteString::formatted("[{},{} -> {},{}]", m_a.x(), m_a.y(), m_b.x(), m_b.y());
}

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Line<T>> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Line<T> const& value)
    {
        return Formatter<FormatString>::format(builder, "[{},{} -> {},{}]"sv, value.a().x(), value.a().y(), value.b().x(), value.b().y());
    }
};

}
