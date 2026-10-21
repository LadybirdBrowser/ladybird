/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Gfx {

template<typename T>
void Point<T>::constrain(Rect<T> const& rect)
{
    m_x = AK::clamp<T>(x(), rect.left(), rect.right() - 1);
    m_y = AK::clamp<T>(y(), rect.top(), rect.bottom() - 1);
}

template<typename T>
[[nodiscard]] Point<T> Point<T>::end_point_for_aspect_ratio(Point<T> const& previous_end_point, float aspect_ratio) const
{
    VERIFY(aspect_ratio > 0);
    T const x_sign = previous_end_point.x() >= x() ? 1 : -1;
    T const y_sign = previous_end_point.y() >= y() ? 1 : -1;
    T dx = AK::abs(previous_end_point.x() - x());
    T dy = AK::abs(previous_end_point.y() - y());
    if (dx > dy) {
        dy = (T)((float)dx / aspect_ratio);
    } else {
        dx = (T)((float)dy * aspect_ratio);
    }
    return { x() + x_sign * dx, y() + y_sign * dy };
}

template<>
ByteString IntPoint::to_byte_string() const
{
    return ByteString::formatted("[{},{}]", x(), y());
}

template<>
ByteString FloatPoint::to_byte_string() const
{
    return ByteString::formatted("[{},{}]", x(), y());
}

}

template class Gfx::Point<int>;
template class Gfx::Point<float>;
