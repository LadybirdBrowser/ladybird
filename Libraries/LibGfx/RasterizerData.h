/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Once.h>
#include <AK/OwnPtr.h>
#include <AK/TypeCasts.h>
#include <AK/kmalloc.h>

namespace Gfx {

// What a rasterizer keeps with a typeface or a font, for as long as the typeface or the font lives.
class RasterizerData {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~RasterizerData() = default;
};

// Holds the rasterizer data of one typeface or font. The first call makes the data, and every call must ask for the
// type that it made.
class RasterizerDataSlot {
    AK_MAKE_NONCOPYABLE(RasterizerDataSlot);
    AK_MAKE_NONMOVABLE(RasterizerDataSlot);

public:
    RasterizerDataSlot() = default;

    template<typename T, typename Callback>
    T& get(Callback make) const
    {
        call_once(m_once, [&] { m_data = make(); });
        return as<T>(*m_data);
    }

private:
    mutable OnceFlag m_once;
    mutable OwnPtr<RasterizerData> m_data;
};

}
