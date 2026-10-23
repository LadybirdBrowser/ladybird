/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/BackingStoreManager.h>
#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositedContext.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/OverlayFade.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/UIOverlays.h>
#include <LibCore/Forward.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/VideoFrameHandle.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Compositor {

class ContextScene;
class ScrollbarController;

struct PendingFrame {
    Gfx::IntRect viewport_rect;
    Gfx::IntRect forced_damage_rect;

    static PendingFrame repainting_everything(Gfx::IntRect viewport_rect) { return { viewport_rect, { {}, viewport_rect.size() } }; }
    static PendingFrame repainting_changes(Gfx::IntRect viewport_rect) { return { viewport_rect, {} }; }
};

struct PreparedFrame {
    Gfx::PaintingSurface* rendered_surface { nullptr };
    i32 bitmap_id { 0 };
    Gfx::IntRect damage_rect;
    double device_pixel_ratio { 1.0 };
};

// Rasterizes the scene of a context into its backing stores and keeps track of the frames it presents.
class ContextPresenter {
    AK_MAKE_NONCOPYABLE(ContextPresenter);
    AK_MAKE_NONMOVABLE(ContextPresenter);

public:
    // What a frame is painted with besides the scene.
    struct FrameContent {
        Compositing::ScrollStateSnapshot const& presented_scroll_state_snapshot;
        ScrollbarController const& scrollbar_controller;
        Optional<Compositing::SpatialNodeIndex> viewport_scroll_node_index;
        CompositedContextResolver const* composited_context_resolver { nullptr };
    };

    ContextPresenter(ContextScene&, OwnedCanvasSurfaces, bool presents_to_client);
    ~ContextPresenter();

    bool presents_to_client() const { return m_presents_to_client; }
    void stop_presenting_to_client() { m_presents_to_client = false; }

    Gfx::IntSize viewport_size() const { return m_viewport_size; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport_size(Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    bool window_resize_in_progress() const { return m_window_resize_in_progress == Compositing::WindowResizingInProgress::Yes; }
    bool should_shrink_backing_stores_after_resize() const;
    void schedule_backing_store_shrink(Function<void()>);
    void finish_window_resize();

    Compositing::UIOverlays const& ui_overlays() const { return m_ui_overlays; }
    void set_ui_overlays(Compositing::UIOverlays);
    Optional<Gfx::IntRect> viewport_rect_for_ui_overlay() const;
    bool ui_overlay_fades_need_frame() const { return m_picture_in_picture_controls_fade.is_fading(); }
    Optional<PendingFrame> advance_ui_overlay_fades(MonotonicTime frame_time);

    Optional<BackingStoreManager::Publication> resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const&, BackingStoreManager::GpuSharing, BackingStoreManager::PublicationMode);
    void invalidate_backing_stores();
    Optional<BackingStoreManager::Publication> add_backing_store_for_pending_frame_if_needed(RefPtr<Gfx::SkiaBackendContext> const&);
    bool has_surplus_backing_stores() const { return m_backing_store_manager.has_surplus_backing_stores(); }
    Vector<i32> retire_idle_surplus_backing_stores() { return m_backing_store_manager.retire_idle_surplus_backing_stores(); }
    void schedule_surplus_backing_store_retirement(Function<void()>);
    bool acknowledge_presented_bitmap(i32 bitmap_id);
    void acknowledge_all_presented_bitmaps() { m_backing_store_manager.release_all_presented_buffers(); }
    void did_finish_gpu_present(i32 bitmap_id);
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const { return m_latest_rendered_surface; }

    bool update_composited_raster_transform(Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
    CompositedContextSurface composited_surface() const;

    void queue_present_frame(PendingFrame);
    void mark_pending_present_frame_scheduled();
    void unschedule_pending_present_frame() { m_pending_present_frame_scheduled = false; }
    bool pending_present_frame_scheduled() const { return m_pending_present_frame_scheduled; }
    bool can_schedule_pending_present_frame_if_unblocked() const;
    Optional<PendingFrame> take_pending_present_frame_if_unblocked();
    bool needs_rasterization() const;
    Optional<Gfx::IntRect> frame_rect_to_repaint() const;
    Optional<Gfx::IntRect> self_present_rect() const;
    bool draws_canvas(Compositing::CanvasId) const;

    Optional<PreparedFrame> prepare_frame(DisplayListPlayerSkia&, PendingFrame, FrameContent const&);
    void did_submit_prepared_frame(Gfx::IntRect);
    bool present_synchronously(DisplayListPlayerSkia&, FrameContent const&);
    void paint_screenshot(DisplayListPlayerSkia&, Gfx::ShareableBitmap&, FrameContent const&);

private:
    // A video frame named by the pool slot acquisition it was decoded into, which is new for every
    // frame even when frames share a surface.
    struct VideoFrameSlot {
        Media::VideoFramePoolID pool_id;
        u32 slot_index { 0 };
        u64 slot_acquisition_id { 0 };

        bool operator==(VideoFrameSlot const&) const = default;
    };

    struct RasterizedFrame {
        NonnullRefPtr<Compositing::DisplayList const> display_list;
        Compositing::AccumulatedVisualContextTree visual_context_tree;
        Compositing::ScrollStateSnapshot scroll_state_snapshot;
        Gfx::IntSize viewport_size;
        HashMap<Compositing::CanvasId, u64> canvas_content_generations;
        HashMap<Compositing::VideoSinkResourceId, Optional<VideoFrameSlot>> video_frame_slots;
    };

    enum class PaintUIOverlay : u8 {
        No,
        Yes,
    };

    bool is_present_blocked() const;
    bool can_render_frame() const;
    void paint_current_display_list(DisplayListPlayerSkia&, Gfx::PaintingSurface&, FrameContent const&, Optional<Gfx::IntRect> damage_rect = {}, PaintUIOverlay = PaintUIOverlay::Yes, bool apply_raster_transform = true);
    Gfx::IntSize raster_size() const;
    Gfx::IntRect raster_damage_rect(Gfx::IntRect) const;
    Gfx::IntRect frame_damage_for(PendingFrame const&, FrameContent const&);
    Gfx::IntRect damage_since_last_raster(Gfx::IntSize viewport_size, FrameContent const&);
    void remember_rasterized_frame(Gfx::IntSize viewport_size, Compositing::ScrollStateSnapshot const& presented_scroll_state_snapshot);
    Optional<VideoFrameSlot> current_video_frame_slot(Compositing::VideoSinkResourceId);
    void stop_backing_store_shrink_timer();
    void stop_surplus_backing_store_retirement_timer();

    ContextScene& m_scene;
    OwnedCanvasSurfaces m_canvas_surfaces;
    bool m_presents_to_client { false };

    Gfx::IntSize m_viewport_size;
    double m_device_pixel_ratio { 1.0 };
    Compositing::WindowResizingInProgress m_window_resize_in_progress { Compositing::WindowResizingInProgress::No };
    Compositing::UIOverlays m_ui_overlays;
    Compositing::OverlayFade m_picture_in_picture_controls_fade;

    BackingStoreManager m_backing_store_manager;
    RefPtr<Gfx::PaintingSurface> m_latest_rendered_surface;
    RefPtr<Gfx::PaintingSurface> m_damage_surface;
    Optional<RasterizedFrame> m_last_rasterized_frame;
    Gfx::FloatSize m_unbounded_raster_scale_of_embedding { 1, 1 };
    Gfx::FloatSize m_raster_scale { 1, 1 };
    Gfx::FloatPoint m_raster_translation;
    RefPtr<Core::Timer> m_backing_store_shrink_timer;
    RefPtr<Core::Timer> m_surplus_backing_store_retirement_timer;

    Optional<PendingFrame> m_pending_present_frame;
    bool m_pending_present_frame_scheduled { false };
    Optional<Gfx::IntRect> m_presented_frame;
};

}
