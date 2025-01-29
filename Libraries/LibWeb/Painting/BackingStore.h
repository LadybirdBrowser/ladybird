/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibGfx/Size.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Web::Painting {

class BackingStore {
    AK_MAKE_NONCOPYABLE(BackingStore);

public:
    virtual Gfx::IntSize size() const = 0;
    virtual Gfx::Bitmap& bitmap() const = 0;

    BackingStore() { }
    virtual ~BackingStore() { }
};

class BitmapBackingStore final : public BackingStore {
public:
    BitmapBackingStore(RefPtr<Gfx::Bitmap>);

    Gfx::IntSize size() const override { return m_bitmap->size(); }
    Gfx::Bitmap& bitmap() const override { return *m_bitmap; }

private:
    RefPtr<Gfx::Bitmap> m_bitmap;
};

#ifdef AK_OS_MACOS
class IOSurfaceBackingStore final : public BackingStore {
public:
    IOSurfaceBackingStore(Core::IOSurfaceHandle&&);

    Gfx::IntSize size() const override;

    Core::IOSurfaceHandle& iosurface_handle() { return m_iosurface_handle; }
    Gfx::Bitmap& bitmap() const override { return *m_bitmap_wrapper; }

private:
    Core::IOSurfaceHandle m_iosurface_handle;
    RefPtr<Gfx::Bitmap> m_bitmap_wrapper;
};
#endif

}
