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
