/*
 * Copyright (c) 2020, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGfx/Point.h>

namespace Gfx {

template<typename T>
class Triangle {
public:
    Triangle(Point<T> a, Point<T> b, Point<T> c)
        : m_a(a)
        , m_b(b)
        , m_c(c)
    {
        m_determinant = (m_b.x() - m_a.x()) * (m_c.y() - m_a.y()) - (m_b.y() - m_a.y()) * (m_c.x() - m_a.x());
    }

    Point<T> a() const { return m_a; }
    Point<T> b() const { return m_b; }
    Point<T> c() const { return m_c; }

    bool contains(Point<T> p) const
    {
        auto x = p.x();
        auto y = p.y();

        auto ax = m_a.x();
        auto bx = m_b.x();
        auto cx = m_c.x();

        auto ay = m_a.y();
        auto by = m_b.y();
        auto cy = m_c.y();

        if (m_determinant * ((bx - ax) * (y - ay) - (by - ay) * (x - ax)) <= 0)
            return false;
        if (m_determinant * ((cx - bx) * (y - by) - (cy - by) * (x - bx)) <= 0)
            return false;
        if (m_determinant * ((ax - cx) * (y - cy) - (ay - cy) * (x - cx)) <= 0)
            return false;
        return true;
    }

    ByteString to_byte_string() const;

private:
    T m_determinant { 0 };
    Point<T> m_a;
    Point<T> m_b;
    Point<T> m_c;
};

}
