/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Rect.h>

namespace Gfx {

template<>
ByteString IntRect::to_byte_string() const
{
    return ByteString::formatted("[{},{} {}x{}]", x(), y(), width(), height());
}

template<>
ByteString FloatRect::to_byte_string() const
{
    return ByteString::formatted("[{},{} {}x{}]", x(), y(), width(), height());
}

}

template class Gfx::Rect<int>;
template class Gfx::Rect<float>;
