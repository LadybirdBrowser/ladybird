/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Painting/BackingStore.h>

namespace Web::Painting {

BitmapBackingStore::BitmapBackingStore(RefPtr<Gfx::Bitmap> bitmap)
    : m_bitmap(move(bitmap))
{
}

#ifdef AK_OS_MACOS
IOSurfaceBackingStore::IOSurfaceBackingStore(Core::IOSurfaceHandle&& iosurface_handle)
    : m_iosurface_handle(move(iosurface_handle))
{
    auto bytes_per_row = m_iosurface_handle.bytes_per_row();
    auto bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size(), bytes_per_row, m_iosurface_handle.data());
    m_bitmap_wrapper = bitmap.release_value();
}

Gfx::IntSize IOSurfaceBackingStore::size() const
{
    return { m_iosurface_handle.width(), m_iosurface_handle.height() };
}
#endif

};
