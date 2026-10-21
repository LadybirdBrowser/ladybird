/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Size.h>

namespace Gfx {

template<>
ByteString IntSize::to_byte_string() const
{
    return ByteString::formatted("[{}x{}]", m_width, m_height);
}

template<>
ByteString FloatSize::to_byte_string() const
{
    return ByteString::formatted("[{}x{}]", m_width, m_height);
}

}

template class Gfx::Size<int>;
template class Gfx::Size<float>;
