/*
 * Copyright (c) 2020, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibGfx/Triangle.h>

namespace Gfx {

template<>
ByteString Triangle<int>::to_byte_string() const
{
    return ByteString::formatted("({},{},{})", m_a, m_b, m_c);
}

template<>
ByteString Triangle<float>::to_byte_string() const
{
    return ByteString::formatted("({},{},{})", m_a, m_b, m_c);
}

}
