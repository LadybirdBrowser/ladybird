/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <Compositor/BackingStoreManager.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorCoreHost.h>
#include <Compositor/DisplayListRasterCache.h>
#include <Compositor/PresentableSurface.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Compositor {

// What the frames of a context are painted with and into: its backing stores, the resources and raster caches that its
// page and the overlays draw with, and the surfaces that the window server shows above its frames. Which frame is
// painted, and what of it, is decided by the presenting logic in Rust.
class ContextRasterTargets {
    AK_MAKE_NONCOPYABLE(ContextRasterTargets);
    AK_MAKE_NONMOVABLE(ContextRasterTargets);

public:
    explicit ContextRasterTargets(OwnedCanvasSurfaces);

    Compositing::DisplayListResourceStorage& resource_storage() { return m_resource_storage; }
    Compositing::DisplayListResourceStorage const& resource_storage() const { return m_resource_storage; }
    void apply_resource_transaction(Compositing::DisplayListResourceTransaction&&);
    DisplayListRasterCache& raster_cache() { return m_raster_cache; }
    OwnedCanvasSurfaces const& canvas_surfaces() const { return m_canvas_surfaces; }

    // The resources that the overlays draw with, apart from those of the page.
    Compositing::DisplayListResourceStorage& overlay_resource_storage() { return m_overlay_resource_storage; }
    DisplayListRasterCache& overlay_raster_cache() { return m_overlay_raster_cache; }
    // Shapes a label of the overlays, in a font that the overlays' resources then hold.
    Optional<OverlayLabel> shape_overlay_label(Optional<StringView> font_family, double device_pixel_ratio, StringView text);

    BackingStoreManager& backing_stores() { return m_backing_stores; }
    BackingStoreManager const& backing_stores() const { return m_backing_stores; }

    // A surface of the pool for the next content shown from it, which the window server is not busy with.
    RefPtr<PresentableSurface> next_presentable_surface(u32 pool, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);
    void drop_presentable_surface_pool(u32 pool) { m_presentable_surface_pools.remove(pool); }

    RefPtr<Media::VideoFrame> current_video_frame(Compositing::VideoSinkResourceId) const;
    bool draws_video_sink(Media::VideoSinkHandle) const;

private:
    Compositing::DisplayListResourceStorage m_resource_storage;
    DisplayListRasterCache m_raster_cache;
    OwnedCanvasSurfaces m_canvas_surfaces;
    Compositing::DisplayListResourceStorage m_overlay_resource_storage;
    DisplayListRasterCache m_overlay_raster_cache;
    BackingStoreManager m_backing_stores;
    HashMap<u32, PresentableSurfacePool> m_presentable_surface_pools;
};

}
