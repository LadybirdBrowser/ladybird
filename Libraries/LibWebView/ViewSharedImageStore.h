/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
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
    void did_allocate_backing_stores(i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store);

    Gfx::SharedImageBuffer const* visible_shared_image_buffer() const;
    Gfx::Bitmap const* visible_bitmap() const;
    Gfx::IntSize visible_bitmap_size() const;

private:
    struct SharedBitmap {
        i32 id { -1 };
        Web::DevicePixelSize last_painted_size;
        OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
    };

    SharedBitmap m_front_bitmap;
    SharedBitmap m_back_bitmap;
    bool m_has_usable_bitmap { false };

    OwnPtr<Gfx::SharedImageBuffer> m_backup_shared_image_buffer;
    Web::DevicePixelSize m_backup_bitmap_size;
};

}