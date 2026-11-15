/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <Compositor/BackingStoreManager.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorCoreHost.h>
#include <Compositor/DisplayListRasterCache.h>
#include <Compositor/DrawnResources.h>
#include <Compositor/PresentableSurface.h>
#include <LibGfx/Size.h>
#include <LibMedia/VideoSinkHandle.h>

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

    DrawnFonts& fonts() { return m_fonts; }
    DrawnFonts const& fonts() const { return m_fonts; }
    DrawnImageFrames& image_frames() { return m_image_frames; }
    DrawnImageFrames const& image_frames() const { return m_image_frames; }
    void retain_only_fonts(ReadonlySpan<u64> font_ids);
    void retain_only_image_frames(ReadonlySpan<u64> image_frame_ids);
    DisplayListRasterCache& raster_cache() { return m_raster_cache; }
    OwnedCanvasSurfaces const& canvas_surfaces() const { return m_canvas_surfaces; }

    // The fonts that the overlays draw with, apart from those of the page.
    DrawnFonts const& overlay_fonts() const { return m_overlay_fonts; }
    DisplayListRasterCache& overlay_raster_cache() { return m_overlay_raster_cache; }
    // Shapes a label of the overlays, in a font that the overlays' fonts then hold.
    Optional<OverlayLabel> shape_overlay_label(Optional<StringView> font_family, double device_pixel_ratio, StringView text);

    BackingStoreManager& backing_stores() { return m_backing_stores; }
    BackingStoreManager const& backing_stores() const { return m_backing_stores; }

    // A surface of the pool for the next content shown from it, which the window server is not busy with.
    RefPtr<PresentableSurface> next_presentable_surface(u32 pool, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);
    void drop_presentable_surface_pool(u32 pool) { m_presentable_surface_pools.remove(pool); }

    // The video sinks that the display list of the context draws. Answers whether these are other sinks than before.
    bool set_drawn_video_sinks(ReadonlySpan<u64> handles);
    HashTable<Media::VideoSinkHandle> const& drawn_video_sinks() const { return m_drawn_video_sinks; }
    bool draws_video_sink(Media::VideoSinkHandle handle) const { return m_drawn_video_sinks.contains(handle); }

private:
    DrawnFonts m_fonts;
    DrawnImageFrames m_image_frames;
    DisplayListRasterCache m_raster_cache;
    OwnedCanvasSurfaces m_canvas_surfaces;
    DrawnFonts m_overlay_fonts;
    DisplayListRasterCache m_overlay_raster_cache;
    BackingStoreManager m_backing_stores;
    HashMap<u32, PresentableSurfacePool> m_presentable_surface_pools;
    HashTable<Media::VideoSinkHandle> m_drawn_video_sinks;
};

}
