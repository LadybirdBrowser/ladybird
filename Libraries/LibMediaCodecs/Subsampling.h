/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/SharedMemoryValue.h>
#include <LibGfx/Size.h>

namespace Media {

struct Subsampling {
public:
    static constexpr Subsampling yuv420() { return { true, true }; }
    static constexpr Subsampling yuv422() { return { true, false }; }
    static constexpr Subsampling yuv444() { return { false, false }; }

    constexpr Subsampling(bool x, bool y)
        : m_x(x)
        , m_y(y)
    {
    }

    Subsampling() = default;

    bool operator==(Subsampling const&) const = default;

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

template<>
struct AK::SharedMemoryCodec<Media::Subsampling> {
    struct Representation {
        u8 x { 0 };
        u8 y { 0 };
    };

    static constexpr Representation encode(Media::Subsampling value) { return { value.x(), value.y() }; }
    static constexpr Optional<Media::Subsampling> decode(Representation representation)
    {
        if (representation.x > 1 || representation.y > 1)
            return {};
        return Media::Subsampling { representation.x == 1, representation.y == 1 };
    }
};
