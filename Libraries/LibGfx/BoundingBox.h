/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Gfx {

template<typename T>
class BoundingBox {
public:
    constexpr BoundingBox() = default;

    constexpr void add_point(T x, T y)
    {
        if (m_has_no_points) {
            m_min_x = m_max_x = x;
            m_min_y = m_max_y = y;
            m_has_no_points = false;
        } else {
            m_min_x = min(m_min_x, x);
            m_min_y = min(m_min_y, y);
            m_max_x = max(m_max_x, x);
            m_max_y = max(m_max_y, y);
        }
    }

    constexpr T x() const { return m_min_x; }
    constexpr T y() const { return m_min_y; }
    constexpr T width() const { return m_max_x - m_min_x; }
    constexpr T height() const { return m_max_y - m_min_y; }

    void add_point(Point<T> point)
    {
        add_point(point.x(), point.y());
    }

    constexpr Rect<T> to_rect() const
    {
        return Rect<T> { x(), y(), width(), height() };
    }

    constexpr operator Rect<T>() const
    {
        return to_rect();
    }

private:
    T m_min_x { 0 };
    T m_min_y { 0 };
    T m_max_x { 0 };
    T m_max_y { 0 };
    bool m_has_no_points { true };
};

using FloatBoundingBox = BoundingBox<float>;
using IntBoundingBox = BoundingBox<int>;

}
