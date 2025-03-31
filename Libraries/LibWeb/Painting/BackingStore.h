/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <LibGfx/Size.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Web::Painting {

class BackingStore : public AtomicRefCounted<BackingStore> {
    AK_MAKE_NONCOPYABLE(BackingStore);

public:
    virtual Gfx::IntSize size() const = 0;
    virtual Gfx::Bitmap& bitmap() const = 0;

    BackingStore() { }
    virtual ~BackingStore() { }
};

class BitmapBackingStore final : public BackingStore {
public:
    static NonnullRefPtr<BitmapBackingStore> create(RefPtr<Gfx::Bitmap> bitmap)
    {
        return adopt_ref(*new BitmapBackingStore(move(bitmap)));
    }

    Gfx::IntSize size() const override { return m_bitmap->size(); }
    Gfx::Bitmap& bitmap() const override { return *m_bitmap; }

private:
    BitmapBackingStore(RefPtr<Gfx::Bitmap>);

    RefPtr<Gfx::Bitmap> m_bitmap;
};

#ifdef AK_OS_MACOS
class IOSurfaceBackingStore final : public BackingStore {
public:
    static NonnullRefPtr<IOSurfaceBackingStore> create(Core::IOSurfaceHandle&& iosurface_handle)
    {
        return adopt_ref(*new IOSurfaceBackingStore(move(iosurface_handle)));
    }

    Gfx::IntSize size() const override;

    Core::IOSurfaceHandle& iosurface_handle() { return m_iosurface_handle; }
    Gfx::Bitmap& bitmap() const override { return *m_bitmap_wrapper; }

private:
    IOSurfaceBackingStore(Core::IOSurfaceHandle&&);

    Core::IOSurfaceHandle m_iosurface_handle;
    RefPtr<Gfx::Bitmap> m_bitmap_wrapper;
};
#endif

}
