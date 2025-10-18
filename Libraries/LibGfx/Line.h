/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
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

        auto u = static_cast<float>(num) / static_cast<float>(denom);
        if (u < 0.0f || u > 1.0f) {
            // Lines are not parallel and don't intersect
            return {};
        }
        auto t = static_cast<float>(cross_product(delta_a, s)) / static_cast<float>(denom);
        if (t < 0.0f || t > 1.0f) {
            // Lines are not parallel and don't intersect
            return {};
        }
        // TODO: round if we're dealing with int
        return Point<T> { m_a.x() + static_cast<T>(t * r.x()), m_a.y() + static_cast<T>(t * r.y()) };
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
        float param = -1.0;
        if (len_sq != 0)
            param = static_cast<float>(delta_a * delta_c + delta_b * delta_d) / static_cast<float>(len_sq);
        if (param < 0)
            return m_a;
        if (param > 1)
            return m_b;
        // TODO: round if we're dealing with int
        return { static_cast<T>(m_a.x() + param * delta_c), static_cast<T>(m_a.y() + param * delta_d) };
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
    // Return a single point representing the intersection of two collinear segments.
    // Compute the 1D overlap via the segment parameter t (dot-product projection onto r),
    // then return the midpoint in t mapped back to 2D. If overlap length is zero, this is
    // the shared endpoint; if there is no overlap, return empty.
    Optional<Point<T>> collinear_intersection_point(Line const& other) const
    {
        auto r = m_b - m_a;
        auto s = other.m_b - other.m_a;

        auto dot = [](auto const& p, auto const& q) {
            return static_cast<double>(p.x()) * static_cast<double>(q.x())
                + static_cast<double>(p.y()) * static_cast<double>(q.y());
        };

        // Parameterize A by t in [0,1]: A(t) = m_a + t * r
        double const rr = dot(r, r);
        if (rr == 0.0) {
            // A is a point, return if it lies on B
            double const ss = dot(s, s);
            if (ss == 0.0)
                return (m_a == other.m_a) ? Optional<Point<T>>(m_a) : Optional<Point<T>> {};
            auto am = m_a - other.m_a;
            double u = dot(am, s) / ss; // parameter on B
            if (u >= 0.0 && u <= 1.0)
                return m_a;
            return {};
        }

        // Project B endpoints onto A's parameter t.
        double t0 = dot(other.m_a - m_a, r) / rr;
        double t1 = dot(other.m_b - m_a, r) / rr;
        if (t0 > t1) {
            double tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        // Overlap of [0,1] with [t0, t1]
        double const start = t0 > 0.0 ? t0 : 0.0;
        double const end = t1 < 1.0 ? t1 : 1.0;
        if (end < start)
            return {};

        double const t_mid = (start + end) / 2.0;
        // TODO: round if we're dealing with int
        return Point<T> {
            m_a.x() + static_cast<T>(t_mid * r.x()),
            m_a.y() + static_cast<T>(t_mid * r.y()),
        };
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
