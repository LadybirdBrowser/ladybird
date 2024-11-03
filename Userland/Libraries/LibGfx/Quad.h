/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibGfx/Triangle.h>

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
        auto top = min(min(m_p1.y(), m_p2.y()), min(m_p3.y(), m_p4.y()));
        auto right = max(max(m_p1.x(), m_p2.x()), max(m_p3.x(), m_p4.x()));
        auto bottom = max(max(m_p1.y(), m_p2.y()), max(m_p3.y(), m_p4.y()));
        auto left = min(min(m_p1.x(), m_p2.x()), min(m_p3.x(), m_p4.x()));
        return { left, top, right - left, bottom - top };
    }

    bool contains(Point<T> point) const
    {
        // FIXME: There's probably a smarter way to do this.
        return Triangle(m_p1, m_p2, m_p3).contains(point)
            || Triangle(m_p1, m_p3, m_p4).contains(point)
            || Triangle(m_p2, m_p4, m_p1).contains(point)
            || Triangle(m_p2, m_p4, m_p3).contains(point);
    }

private:
    Point<T> m_p1;
    Point<T> m_p2;
    Point<T> m_p3;
    Point<T> m_p4;
};

}
