/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Size.h>

namespace Media {

struct Subsampling {
public:
    Subsampling(bool x, bool y)
        : m_x(x)
        , m_y(y)
    {
    }

    Subsampling() = default;

    bool x() const { return m_x; }
    bool y() const { return m_y; }

    static u32 subsampled_size(bool subsampled, u32 size)
    {
        u32 subsampled_as_int = static_cast<u32>(subsampled);
        return (size + subsampled_as_int) >> subsampled_as_int;
    }

    template<Integral T>
    Gfx::Size<T> subsampled_size(Gfx::Size<T> size) const
    {
        return {
            subsampled_size(x(), size.width()),
            subsampled_size(y(), size.height())
        };
    }

private:
    bool m_x = false;
    bool m_y = false;
};

}
