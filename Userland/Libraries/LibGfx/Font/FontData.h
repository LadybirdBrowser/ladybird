/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <LibCore/Resource.h>
#include <LibGfx/Forward.h>

namespace Gfx {

class FontData {
public:
    static NonnullOwnPtr<FontData> create_from_byte_buffer(ByteBuffer&&);
    static NonnullOwnPtr<FontData> create_from_resource(Core::Resource const&);

    ReadonlyBytes bytes() const;

private:
    FontData(ByteBuffer&& byte_buffer);
    FontData(NonnullRefPtr<Core::Resource> resource);

    Variant<ByteBuffer, NonnullRefPtr<Core::Resource>> m_data;
};

}
