/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>

namespace Gfx {

struct CMYK {
    u8 c;
    u8 m;
    u8 y;
    u8 k;

    int operator<=>(CMYK const&) const = default;
};

class CMYKBitmap : public RefCounted<CMYKBitmap> {
public:
    static ErrorOr<NonnullRefPtr<CMYKBitmap>> create_with_size(IntSize const& size);

    IntSize const& size() const { return m_size; }

    [[nodiscard]] CMYK* scanline(int y);
    [[nodiscard]] CMYK const* scanline(int y) const;

    [[nodiscard]] CMYK* begin();
    [[nodiscard]] CMYK* end();
    [[nodiscard]] size_t data_size() const { return m_data.size(); }

    ErrorOr<RefPtr<Bitmap>> to_low_quality_rgb() const;

private:
    CMYKBitmap(IntSize const& size, ByteBuffer data)
        : m_size(size)
        , m_data(move(data))
    {
    }

    IntSize m_size;
    ByteBuffer m_data;

    mutable RefPtr<Bitmap> m_rgb_bitmap;
};

inline CMYK* CMYKBitmap::scanline(int y)
{
    VERIFY(y >= 0 && y < m_size.height());
    return reinterpret_cast<CMYK*>(m_data.data() + y * m_size.width() * sizeof(CMYK));
}

inline CMYK const* CMYKBitmap::scanline(int y) const
{
    VERIFY(y >= 0 && y < m_size.height());
    return reinterpret_cast<CMYK const*>(m_data.data() + y * m_size.width() * sizeof(CMYK));
}

inline CMYK* CMYKBitmap::begin()
{
    return reinterpret_cast<CMYK*>(m_data.data());
}

inline CMYK* CMYKBitmap::end()
{
    return reinterpret_cast<CMYK*>(m_data.data() + data_size());
}

}
