/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/Size.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API ViewSharedImageStore {
public:
    ViewSharedImageStore();
    ~ViewSharedImageStore();

    void reset();
    void clear_backup_bitmap();

    void did_paint(i32 bitmap_id, Gfx::IntSize size, Function<void()> on_ready_to_paint = {});
    void did_allocate_backing_stores(i32 front_bitmap_id, Gfx::SharedImagePayload front_backing_store, i32 back_bitmap_id, Gfx::SharedImagePayload back_backing_store);

    Gfx::Bitmap const* visible_bitmap() const;
    Gfx::IntSize visible_bitmap_size() const;
    void* visible_bitmap_iosurface_ref() const;

private:
    struct SharedBitmap;

    OwnPtr<SharedBitmap> m_front_bitmap;
    OwnPtr<SharedBitmap> m_back_bitmap;
    bool m_has_usable_bitmap { false };

    RefPtr<Gfx::Bitmap> m_backup_bitmap;
    Web::DevicePixelSize m_backup_bitmap_size;
};

}
