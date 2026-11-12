/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/RemoteSublayer.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

// A layer that this process commits frames into, and that the UI shows in a view of its own by the layer's host handle.
// Frames then reach the screen without passing through the UI.
class RemoteLayer {
    AK_MAKE_NONCOPYABLE(RemoteLayer);
    AK_MAKE_NONMOVABLE(RemoteLayer);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~RemoteLayer() = default;

    virtual Compositing::RemoteLayerHostHandle host_handle() const = 0;

    using Content = Variant<NonnullRefPtr<Media::VideoFrame>, NonnullRefPtr<PresentableSurface>>;
    virtual bool can_show(WindowServerPlacement const&, Content const&, double device_pixel_ratio) const = 0;
    virtual bool can_show_high_dynamic_range() const { return false; }

    struct Frame {
        // Without a store, the frame shown before stays, and only the sublayers change.
        Optional<PresentableSurface::Lease> store;
        Gfx::IntSize painted_size;
        double device_pixel_ratio { 1.0 };
        Optional<Gfx::IntSize> fit_within;
        Vector<RemoteSublayer> sublayers;
    };
    // Shows the part of the store that the frame was painted into, at the top left of the layer at its own size, or
    // scaled to fit within the size of the view given, in device pixels, with the sublayers above it, bottom to top.
    virtual void show_frame(Frame) = 0;
    // Places the frame on display again, as when the view that it is scaled into changes size.
    virtual void place_frame(Optional<Gfx::IntSize> fit_within) = 0;

protected:
    RemoteLayer() = default;
};

}
