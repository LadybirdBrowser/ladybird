/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Once.h>
#include <AK/OwnPtr.h>
#include <AK/kmalloc.h>

namespace Gfx {

// What a rasterizer keeps with a font, typeface, or decoded image frame for its lifetime.
class RasterizerData {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~RasterizerData() = default;
};

// The rasterizer defines T. LibGfx can destroy it through RasterizerData while T is incomplete.
template<typename T>
class RasterizerDataSlot {
    AK_MAKE_NONCOPYABLE(RasterizerDataSlot);
    AK_MAKE_NONMOVABLE(RasterizerDataSlot);

public:
    RasterizerDataSlot() = default;

    template<typename Callback>
    T& get(Callback make) const
    requires(SameAs<decltype(make()), NonnullOwnPtr<T>>)
    {
        call_once(m_once, [&] { m_data = make(); });
        return static_cast<T&>(*m_data);
    }

private:
    mutable OnceFlag m_once;
    mutable OwnPtr<RasterizerData> m_data;
};

}
