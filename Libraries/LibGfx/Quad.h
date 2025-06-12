/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibGfx/Point.h>

namespace Gfx {

template<typename T>
class Quad {
public:
    Quad(Point<T> p1, Point<T> p2, Point<T> p3, Point<T> p4)
        : m_p1(p1)
        , m_p2(p2)
        , m_p3(p3)
        , m_p4(p4)
    {
    }

    Point<T> const& p1() const { return m_p1; }
    Point<T> const& p2() const { return m_p2; }
    Point<T> const& p3() const { return m_p3; }
    Point<T> const& p4() const { return m_p4; }

    Rect<T> bounding_rect() const
    {
        T left = min(min(m_p1.x(), m_p2.x()), min(m_p3.x(), m_p4.x()));
        T right = max(max(m_p1.x(), m_p2.x()), max(m_p3.x(), m_p4.x()));
        T width = right - left;

        T top = min(min(m_p1.y(), m_p2.y()), min(m_p3.y(), m_p4.y()));
        T bottom = max(max(m_p1.y(), m_p2.y()), max(m_p3.y(), m_p4.y()));
        T height = bottom - top;

        return { left, top, width, height };
    }

    bool contains(Point<T> point) const
    {
        // Even-Odd algorithm: https://www.geeksforgeeks.org/even-odd-method-winding-number-method-inside-outside-test-of-a-polygon/
        //
        // 1. "Constructing a line segment between the point (P) to be examined and a known point outside the polygon"
        //      - We're using horizontal line from (point.x, point.y) to (bounding_rect().left + bounding_rect().width + 1, point.y)
        //        (i.e. just +1 to right of furthest-right point in quad)
        //
        // 2. "The number of times the line segment intersects the polygon boundary is then counted."
        //      - We count the line's intersections with the quad by checking each quad edge for intersection (1-2, 2-3, 3-4, 4-1)
        //
        // 3. "The point (P) is an internal point if the number of polygon edges intersected by this line is odd;
        //    otherwise, the point is an external point."

        u8 intersections_with_quad = 0;
        auto const quad_points = AK::Array { &m_p1, &m_p2, &m_p3, &m_p4, &m_p1 };
        for (size_t i = 0, j = 1; i < 4 && j < 5; i++, j++) {
            if ((quad_points[i]->y() > point.y()) == (quad_points[j]->y() > point.y())) {
                continue;
            }

            T x_coord_of_intersection_with_edge = (quad_points[j]->x() - quad_points[i]->x()) * (point.y() - quad_points[i]->y()) / (quad_points[j]->y() - quad_points[i]->y()) + quad_points[i]->x();
            if (point.x() < x_coord_of_intersection_with_edge) {
                intersections_with_quad++;
            }
        }

        return (intersections_with_quad % 2) == 1;
    }

private:
    Point<T> m_p1;
    Point<T> m_p2;
    Point<T> m_p3;
    Point<T> m_p4;
};

}
