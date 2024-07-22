/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontData.h>

namespace Gfx {

NonnullOwnPtr<FontData> FontData::create_from_byte_buffer(ByteBuffer&& byte_buffer)
{
    return adopt_own(*new FontData(move(byte_buffer)));
}

NonnullOwnPtr<FontData> FontData::create_from_resource(Core::Resource const& resource)
{
    return adopt_own(*new FontData(resource));
}

ReadonlyBytes FontData::bytes() const
{
    return m_data.visit(
        [&](ByteBuffer const& byte_buffer) { return byte_buffer.bytes(); },
        [&](Core::Resource const& resource) {
            return resource.data();
        });
}

FontData::FontData(ByteBuffer&& byte_buffer)
    : m_data(move(byte_buffer))
{
}

FontData::FontData(NonnullRefPtr<Core::Resource> resource)
    : m_data(move(resource))
{
}

}
