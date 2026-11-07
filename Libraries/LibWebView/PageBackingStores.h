/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/Size.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The backing stores that the Compositor presents a page's frames in, as seen by whoever shows those frames. One of them
// is the front backing store, which holds the frame on display. The Compositor paints into the others.
class WEBVIEW_API PageBackingStores {
public:
    struct Frame {
        Gfx::SharedImageBuffer const& shared_image_buffer;
        Gfx::IntSize size;
        double device_pixel_ratio { 1.0 };
        // Empty for a frame kept from backing stores that are gone.
        Optional<i32> bitmap_id;
    };

    // The frame to show: the one in the front backing store, or until that has one, the frame kept as a backup.
    Optional<Frame> displayed_frame() const;

    bool has_usable_front_bitmap() const { return m_has_usable_front_bitmap; }
    i32 front_bitmap_id() const { return m_front_bitmap.id; }
    Vector<Gfx::SharedImageBuffer const*> shared_image_buffers() const;

    // Replaces every backing store. The first of the new ones becomes the front backing store.
    void install(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>);
    void add(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage>);
    void retire(ReadonlySpan<i32> bitmap_ids);

    struct PresentationResult {
        bool did_swap { false };
        // The backing store the Compositor may paint into again.
        i32 bitmap_id_to_release { -1 };
    };
    // Makes the backing store with this ID the front backing store, unless it is no longer one of ours.
    PresentationResult present(i32 bitmap_id, Gfx::IntSize, double device_pixel_ratio);

    // Keeps the frame on display, so there is something to show until a new frame arrives.
    void keep_front_as_backup();
    void clear_backup() { m_backup = {}; }

    // Forgets every backing store, other than the frame kept as a backup.
    void clear();

private:
    struct SharedBitmap {
        i32 id { -1 };
        Web::DevicePixelSize last_painted_size;
        double last_painted_device_pixel_ratio { 1.0 };
        OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
    };

    SharedBitmap m_front_bitmap;
    Vector<SharedBitmap> m_other_bitmaps;
    bool m_has_usable_front_bitmap { false };
    SharedBitmap m_backup;
};

}
