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
#include <Compositor/ViewportScrollbarController.h>
#include <LibCore/Forward.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/SmoothScrollAnimation.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWebView/Forward.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Web {

struct MouseEvent;
struct PinchEvent;

}

namespace Web::Painting {

class DisplayListPlayerSkia;

}

namespace Compositor {

class CompositorStateWebContentClient;
using CompositedContextResolver = Function<RefPtr<Gfx::PaintingSurface>(Web::Compositor::CompositorContextId)>;

class ContextState {
    AK_MAKE_NONCOPYABLE(ContextState);
    AK_MAKE_NONMOVABLE(ContextState);

public:
    struct PendingFrame {
        Gfx::IntRect viewport_rect;
        Gfx::IntRect forced_damage_rect;

        static PendingFrame repainting_everything(Gfx::IntRect viewport_rect) { return { viewport_rect, { {}, viewport_rect.size() } }; }
        static PendingFrame repainting_changes(Gfx::IntRect viewport_rect) { return { viewport_rect, {} }; }
    };

    struct AsyncScrollResult {
        Web::Compositor::AsyncScrollEnqueueResult enqueue_result;
        Optional<PendingFrame> frame_to_present;
    };

    struct ContextUpdateResult {
        bool accepted { false };
        Optional<PendingFrame> frame_to_present;
        bool should_request_rendering_update { false };
    };

    struct PreparedFrame {
        Gfx::PaintingSurface* rendered_surface { nullptr };
        i32 bitmap_id { 0 };
        Gfx::IntRect damage_rect;
    };

    ContextState(Optional<u64> page_id, CompositorStateWebContentClient&, Web::Painting::CanvasSurfaceRegistry const&, bool async_scrolling_enabled, Function<void(Gfx::IntRect)> schedule_caret_repaint = {});
    ~ContextState();

    bool is_owned_by(CompositorStateWebContentClient const&) const;
    CompositorStateWebContentClient& web_content_client() const { return m_web_content_client; }
    void request_rendering_update();
    void dispatch_mouse_event_to_web_content(Web::MouseEvent const&);

    bool presents_to_client() const { return m_presents_to_client; }
    void stop_presenting_to_client();
    void did_stop_presenting_to_client_if_needed(bool was_presenting_to_client, bool will_present_to_client);

    void set_parent_context(Optional<Web::Compositor::CompositorContextId>);
    Optional<Web::Compositor::CompositorContextId> parent_context_id() const { return m_parent_context_id; }
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const { return m_latest_rendered_surface; }

    void apply_display_list_resource_transaction(Web::Painting::DisplayListResourceTransaction&&);
    void update_image_frame_resources(Vector<Web::Painting::DisplayListImageFrameResource>);
    void install_display_list_update(
        NonnullRefPtr<Web::Painting::DisplayList>,
        Web::Painting::AccumulatedVisualContextTree,
        Web::Painting::ScrollStateSnapshot&&);
    void update_visual_context_tree(Web::Painting::AccumulatedVisualContextTree, Web::Painting::DisplayListResourceTransaction&&);
    void update_scroll_state(Web::Painting::ScrollStateSnapshot&&);
    void set_video_sink(Web::Painting::VideoSinkResourceId, RefPtr<Media::VideoSink>);
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_display_list_resource_storage.video_sink_handles(); }

    void invalidate_wheel_event_listener_state(u64 generation);
    ContextUpdateResult handle_mouse_event(Web::MouseEvent const&);
    ContextUpdateResult handle_pinch_event(Web::PinchEvent const&);
    AsyncScrollResult async_scroll_by(
        Web::UniqueNodeID document_id,
        Gfx::FloatPoint position,
        Gfx::FloatPoint delta,
        Gfx::IntRect viewport_rect,
        Web::Compositor::SnapContainerHandling,
        Web::Compositor::AsyncScrollOperationTracking);
    AsyncScrollResult smooth_scroll_to(Web::Compositor::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, double device_pixels_per_css_pixel, Web::Compositor::ScrollAnimationKind);
    void cancel_smooth_scroll(Web::Compositor::AsyncScrollNodeStableID);
    Optional<Gfx::IntRect> advance_smooth_scroll_animations(MonotonicTime now);
    bool has_active_smooth_scroll_animations() const { return !m_smooth_scroll_animations.is_empty(); }
    bool advance_visual_animations(MonotonicTime now);
    bool has_active_visual_animations() const { return m_has_active_visual_animations; }
    Web::Painting::AccumulatedVisualContextTree const& visual_context_tree_for_testing() const { return current_visual_context_tree(); }
    bool has_sampled_visual_animation_values_for_testing() const { return m_sampled_visual_context_tree.has_value(); }
    u64 visual_context_tree_copy_count_for_testing() const { return m_visual_context_tree_copy_count; }
    Gfx::IntRect caret_damage_rect_for_testing() { return caret_damage_rect(); }
    ContextUpdateResult async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::Compositor::SnapContainerHandling);
    Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates();

    void viewport_size_updated(Gfx::IntSize, Web::Compositor::WindowResizingInProgress);
    bool set_paused_debugger_overlay(bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<WebView::PausedDebuggerOverlayAction> hovered_action);
    bool paused_debugger_overlay_visible() const { return m_paused_debugger_overlay_visible; }
    Optional<Gfx::IntRect> viewport_rect_for_ui_overlay() const;
    bool should_shrink_backing_stores_after_resize() const;
    void schedule_backing_store_shrink(Function<void()>);
    void finish_window_resize();
    Optional<BackingStoreManager::Publication> resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const&, BackingStoreManager::GpuSharing);
    void invalidate_backing_stores();

    bool set_display_metadata(Optional<u64> display_id, double refresh_rate);
    Optional<u64> display_id() const { return m_display_id; }
    double display_refresh_rate() const { return m_display_refresh_rate; }
    bool set_visibility(Web::Compositor::ContextVisibility);
    Web::Compositor::ContextVisibility visibility() const { return m_visibility; }

    void queue_present_frame(PendingFrame);
    Optional<Gfx::IntRect> pending_present_frame_viewport_rect() const;
    void mark_pending_present_frame_scheduled();
    void unschedule_pending_present_frame() { m_pending_present_frame_scheduled = false; }
    bool has_pending_present_frame_scheduled_on(Optional<u64> display_id) const;
    bool can_schedule_pending_present_frame_if_unblocked() const;
    Optional<PendingFrame> take_pending_present_frame_if_unblocked();
    bool needs_rasterization() const;
    Optional<Gfx::IntRect> frame_rect_to_repaint() const;
    Optional<Gfx::IntRect> video_present_rect() const;
    Optional<PreparedFrame> prepare_frame(Web::Painting::DisplayListPlayerSkia&, PendingFrame, CompositedContextResolver const*);
    void did_submit_prepared_frame(Gfx::IntRect);
    bool present_synchronously(Web::Painting::DisplayListPlayerSkia&, CompositedContextResolver const*);
    bool can_paint_screenshot(Gfx::ShareableBitmap&) const;
    void paint_screenshot(Web::Painting::DisplayListPlayerSkia&, Gfx::ShareableBitmap&, CompositedContextResolver const*);
    bool acknowledge_presented_bitmap(i32 bitmap_id);
    void did_finish_gpu_present(i32 bitmap_id);

private:
    struct ActiveSmoothScrollAnimation {
        Web::Compositor::AsyncScrollNodeStableID stable_node_id;
        Web::Compositor::AsyncScrollOperationID operation_id;
        Web::Compositor::SmoothScrollAnimation animation;
        MonotonicTime started_at;
    };

    struct VisualViewportScrollDelta {
        Web::Compositor::AsyncScrollOffset scroll_offset;
        Gfx::FloatPoint consumed_delta;
    };

    struct RasterizedFrame {
        NonnullRefPtr<Web::Painting::DisplayList const> display_list;
        Web::Painting::AccumulatedVisualContextTree visual_context_tree;
        Web::Painting::ScrollStateSnapshot scroll_state_snapshot;
        Gfx::IntSize viewport_size;
        HashMap<Web::Painting::CanvasId, u64> canvas_content_generations;
    };

    void stop_backing_store_shrink_timer();
    Web::Painting::AccumulatedVisualContextTree const& current_visual_context_tree() const;
    Optional<Gfx::FloatPoint> viewport_scroll_offset_from(Vector<Web::Compositor::AsyncScrollOffset> const&) const;
    Optional<float> visual_viewport_scale_for_compositing() const;
    Optional<VisualViewportScrollDelta> apply_visual_viewport_scroll_delta(Gfx::FloatPoint);
    Optional<Gfx::FloatPoint> reapply_pending_async_scroll_offsets(Vector<Web::Compositor::AsyncScrollOffset> const&);
    void store_pending_async_scroll_offsets(Vector<Web::Compositor::AsyncScrollOffset> const&, Optional<Web::Compositor::AsyncScrollOperationID> = {});
    void cancel_smooth_scroll_taken_over_by_user_input(Web::Compositor::AsyncScrollNodeID);
    void note_user_scroll_gesture_end_if_drag_ended(bool was_dragging_viewport_scrollbar);
    Optional<PendingFrame> apply_viewport_scrollbar_drag(ViewportScrollbarController::Drag const&);
    void rebuild_wheel_hit_test_targets();
    void discard_sampled_visual_context_tree();
    void invalidate_visual_context_tree_for_compositing();
    bool is_present_blocked() const;
    bool can_render_frame() const;
    Web::Painting::AccumulatedVisualContextTree const& visual_context_tree_for_compositing();
    enum class PaintUIOverlay : u8 {
        No,
        Yes,
    };
    void paint_current_display_list(Web::Painting::DisplayListPlayerSkia&, Gfx::PaintingSurface&, CompositedContextResolver const*, Optional<Gfx::IntRect> damage_rect = {}, PaintUIOverlay = PaintUIOverlay::Yes);
    Gfx::IntRect frame_damage_for(PendingFrame const&);
    Gfx::IntRect damage_since_last_raster(Gfx::IntSize viewport_size);
    void remember_rasterized_frame(Gfx::IntSize viewport_size);
    void update_caret_blink_timer();
    void schedule_next_caret_blink();
    Gfx::IntRect caret_damage_rect();

    CompositorStateWebContentClient& m_web_content_client;
    Web::Painting::CanvasSurfaceRegistry const& m_canvas_surface_registry;
    Optional<u64> m_page_id;
    bool const m_async_scrolling_enabled { true };

    bool m_presents_to_client { false };
    Optional<Web::Compositor::CompositorContextId> m_parent_context_id;

    RefPtr<Web::Painting::DisplayList const> m_display_list;
    Optional<Web::Painting::AccumulatedVisualContextTree> m_visual_context_tree;
    Optional<Web::Painting::AccumulatedVisualContextTree> m_visual_context_tree_for_compositing;
    Optional<Web::Painting::AccumulatedVisualContextTree> m_sampled_visual_context_tree;
    u64 m_visual_context_tree_copy_count { 0 };
    bool m_has_active_visual_animations { false };
    Web::Painting::DisplayListResourceStorage m_display_list_resource_storage;
    Web::Painting::ScrollStateSnapshot m_scroll_state_snapshot;
    BackingStoreManager m_backing_store_manager;
    RefPtr<Gfx::PaintingSurface> m_latest_rendered_surface;
    RefPtr<Gfx::PaintingSurface> m_damage_surface;
    Optional<RasterizedFrame> m_last_rasterized_frame;

    Web::Compositor::AsyncScrollTree m_async_scroll_tree;
    ViewportScrollbarController m_viewport_scrollbar_controller;

    Vector<Web::Compositor::AsyncScrollOffset> m_pending_async_scroll_offsets;
    Vector<Web::Compositor::AsyncScrollOperationID> m_completed_async_scroll_operation_ids;
    Vector<Web::Compositor::AsyncScrollOperationID> m_async_scroll_operation_ids_taken_over_by_user_input;
    bool m_user_scroll_gesture_ended { false };
    Vector<ActiveSmoothScrollAnimation> m_smooth_scroll_animations;
    Web::Compositor::AsyncScrollOperationID m_next_async_scroll_operation_id { 0 };
    Gfx::IntRect m_async_scrolling_viewport_rect;
    bool m_has_async_scrolling_state { false };
    bool m_can_accept_async_wheel_events { false };
    bool m_has_blocking_wheel_event_listeners { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    Web::Compositor::WheelRoutingAdmission m_wheel_routing_admission { Web::Compositor::WheelRoutingAdmission::NoAsyncScrollingState };
    Optional<Web::Painting::TransformWithOrigin> m_async_visual_viewport_transform;
    Optional<i64> m_visual_animation_sample_time_ns;

    Gfx::IntSize m_viewport_size;
    bool m_paused_debugger_overlay_visible { false };
    double m_paused_debugger_overlay_device_pixel_ratio { 1.0 };
    Optional<String> m_paused_debugger_overlay_font_family;
    Optional<WebView::PausedDebuggerOverlayAction> m_paused_debugger_overlay_hovered_action;
    Web::Compositor::WindowResizingInProgress m_window_resize_in_progress { Web::Compositor::WindowResizingInProgress::No };
    RefPtr<Core::Timer> m_backing_store_shrink_timer;
    Function<void(Gfx::IntRect)> m_schedule_caret_repaint;
    RefPtr<Core::Timer> m_caret_blink_timer;
    Optional<i64> m_caret_blink_cycle_start_time_ns;
    Optional<u64> m_display_id;
    double m_display_refresh_rate { 60.0 };
    Web::Compositor::ContextVisibility m_visibility { Web::Compositor::ContextVisibility::Visible };

    Optional<PendingFrame> m_pending_present_frame;
    bool m_pending_present_frame_scheduled { false };
    Optional<Gfx::IntRect> m_presented_frame;
    Optional<i32> m_gpu_present_bitmap_id_awaiting_completion;
};

}
