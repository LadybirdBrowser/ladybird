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
#include <AK/Span.h>
#include <AK/Vector.h>
#include <Compositor/BackingStoreManager.h>
#include <Compositor/FramePacer.h>
#include <Compositor/ScrollSnapController.h>
#include <Compositor/ScrollbarController.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/CompositedContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollFling.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Scrolling/SmoothScrollAnimation.h>
#include <LibCompositing/Scrolling/WheelGestureIdentity.h>
#include <LibCompositing/Types.h>
#include <LibCore/Forward.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Compositing {

class DisplayListPlayerSkia;

}

namespace Compositor {

class CompositorStateWebContentClient;
using Compositing::CompositedContextResolver;

class ContextState {
    AK_MAKE_NONCOPYABLE(ContextState);
    AK_MAKE_NONMOVABLE(ContextState);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct PendingFrame {
        Gfx::IntRect viewport_rect;
        Gfx::IntRect forced_damage_rect;

        static PendingFrame repainting_everything(Gfx::IntRect viewport_rect) { return { viewport_rect, { {}, viewport_rect.size() } }; }
        static PendingFrame repainting_changes(Gfx::IntRect viewport_rect) { return { viewport_rect, {} }; }
    };

    struct AsyncScrollResult {
        Compositing::AsyncScrollEnqueueResult enqueue_result;
        Optional<PendingFrame> frame_to_present;
    };

    struct ContextUpdateResult {
        bool accepted { false };
        Optional<PendingFrame> frame_to_present;
        bool should_request_rendering_update { false };
        Optional<Web::ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor {};
    };

    struct PreparedFrame {
        Gfx::PaintingSurface* rendered_surface { nullptr };
        i32 bitmap_id { 0 };
        Gfx::IntRect damage_rect;
    };

    ContextState(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&, Compositing::CanvasSurfaceRegistry const&, Function<void(Gfx::IntRect)> schedule_caret_repaint = {});
    ~ContextState();

    bool is_owned_by(CompositorStateWebContentClient const&) const;
    CompositorStateWebContentClient& web_content_client() const { return m_web_content_client; }
    void request_rendering_update();
    void dispatch_mouse_event_to_web_content(Web::MouseEvent const&);
    void dispatch_key_event_to_web_content(Web::KeyEvent const&);
    bool can_dispatch_input_to_web_content() const { return m_page_id.has_value(); }

    bool presents_to_client() const { return m_presents_to_client; }
    void stop_presenting_to_client();
    void did_stop_presenting_to_client_if_needed(bool was_presenting_to_client, bool will_present_to_client);

    void set_parent_context(Optional<Web::CompositorContextId>);
    Optional<Web::CompositorContextId> parent_context_id() const { return m_parent_context_id; }
    RefPtr<Gfx::PaintingSurface> latest_rendered_surface() const { return m_latest_rendered_surface; }
    bool update_composited_raster_transform(Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform);
    Compositing::CompositedContextSurface composited_surface() const;

    void apply_display_list_resource_transaction(Compositing::DisplayListResourceTransaction&&);
    void install_display_list_update(
        NonnullRefPtr<Compositing::DisplayList>,
        Compositing::AccumulatedVisualContextTree,
        Compositing::ScrollStateSnapshot&&);
    void update_visual_context_tree(Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void update_scroll_state(Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void set_video_sink(Compositing::VideoSinkResourceId, RefPtr<Media::VideoSink>);
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_display_list_resource_storage.video_sink_handles(); }

    void invalidate_wheel_event_listener_state(u64 generation);
    void invalidate_keyboard_scroll_state(u64 generation);
    ContextUpdateResult handle_key_event(Web::KeyEvent const&);
    ContextUpdateResult handle_mouse_event(Web::MouseEvent const&);
    // The UI's wheel deltas are in CSS pixels; this scales them for the scroll tree, which works in device pixels.
    ContextUpdateResult handle_wheel_event(Web::MouseEvent const&, Optional<MonotonicTime> now_for_testing = {});
    ContextUpdateResult handle_pinch_event(Web::PinchEvent const&);
    AsyncScrollResult async_scroll_by(
        Web::UniqueNodeID document_id,
        Gfx::FloatPoint position,
        Gfx::FloatPoint delta,
        Gfx::IntRect viewport_rect,
        Web::WheelDeltaPrecision,
        Web::ScrollGesturePhase,
        u32 modifiers,
        Compositing::AsyncScrollOperationTracking,
        Optional<MonotonicTime> now_for_testing = {});
    AsyncScrollResult smooth_scroll_to(Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator);
    void cancel_smooth_scroll(Web::AsyncScrollNodeStableID);
    Optional<Gfx::IntRect> advance_smooth_scroll_animations(MonotonicTime now);
    bool has_active_smooth_scroll_animations() const { return !m_smooth_scroll_animations.is_empty(); }
    bool needs_animation_frames() { return has_active_smooth_scroll_animations() || has_active_scroll_fling() || visual_animations_need_frame(); }

    bool has_active_scroll_fling() const { return m_scroll_fling.has_value(); }
    struct ScrollFlingStep {
        Web::MouseEvent event;
        ContextUpdateResult result;
    };
    Optional<ScrollFlingStep> take_scroll_fling_step(MonotonicTime frame_time);
    Optional<ScrollFlingStep> end_scroll_fling(MonotonicTime now);
    // Starts a fling if the gesture that the event ends was a flick. Use only on platforms that send no momentum
    // scroll events.
    bool start_scroll_fling_if_flicked(Web::MouseEvent const& gesture_end_event, Optional<MonotonicTime> now_for_testing = {});
    // Ends the fling before a wheel event from a new gesture or other input.
    Optional<ScrollFlingStep> interrupt_scroll_fling(Web::MouseEvent const&, Optional<MonotonicTime> now_for_testing = {});
    bool advance_visual_animations(MonotonicTime now);
    bool has_active_visual_animations() const { return m_has_active_visual_animations; }
    bool visual_animations_need_frame();
    u64 visual_context_tree_copy_count_for_testing() const { return m_visual_context_tree_copy_count; }
    Optional<Web::AsyncScrollNodeStableID> latched_wheel_scroller_for_testing() const;
    ContextUpdateResult async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, Optional<MonotonicTime> now_for_testing = {});
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates();
    bool has_pending_async_scroll_updates() const;
    // A gesture whose steps this context chains ends once they stop arriving, which is reported to WebContent so
    // that it settles the gesture then rather than on a clock of its own.
    void end_scroll_step_gestures_whose_input_ran_out(MonotonicTime now);

    void viewport_size_updated(Gfx::IntSize, Compositing::WindowResizingInProgress);
    bool set_paused_debugger_overlay(bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<Compositing::PausedDebuggerOverlayAction> hovered_action);
    bool paused_debugger_overlay_visible() const { return m_paused_debugger_overlay_visible; }
    Optional<Gfx::IntRect> viewport_rect_for_ui_overlay() const;
    bool should_shrink_backing_stores_after_resize() const;
    void schedule_backing_store_shrink(Function<void()>);
    void finish_window_resize();
    Optional<BackingStoreManager::Publication> resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const&, BackingStoreManager::GpuSharing);
    void invalidate_backing_stores();
    Optional<BackingStoreManager::Publication> add_backing_store_for_pending_frame_if_needed(RefPtr<Gfx::SkiaBackendContext> const&);
    bool has_surplus_backing_stores() const { return m_backing_store_manager.has_surplus_backing_stores(); }
    Vector<i32> retire_idle_surplus_backing_stores() { return m_backing_store_manager.retire_idle_surplus_backing_stores(); }
    void schedule_surplus_backing_store_retirement(Function<void()>);

    bool set_display_metadata(Optional<u64> display_id, double refresh_rate);
    Optional<u64> display_id() const { return m_display_id; }
    double display_refresh_rate() const { return m_display_refresh_rate; }
    bool set_visibility(Compositing::ContextVisibility);
    Compositing::ContextVisibility visibility() const { return m_visibility; }

    bool request_rendering_opportunity(double maximum_frames_per_second);
    bool rendering_opportunity_requested() const { return m_rendering_opportunity_requested; }
    bool window_resize_in_progress() const { return m_window_resize_in_progress == Compositing::WindowResizingInProgress::Yes; }
    bool is_present_blocked() const;
    double rendering_opportunity_frame_interval(double display_refresh_rate) const;
    bool rendering_opportunity_is_due(MonotonicTime frame_time, double display_refresh_rate) const;
    void did_deliver_rendering_opportunity(MonotonicTime frame_time);

    bool request_clock_tick(double maximum_frames_per_second);
    bool clock_tick_requested() const { return m_clock_tick_requested; }
    double clock_tick_interval(double display_refresh_rate) const { return m_clock_tick_pacer.frame_interval(display_refresh_rate); }
    bool clock_tick_is_due(MonotonicTime frame_time, double display_refresh_rate) const { return m_clock_tick_pacer.is_due(frame_time, display_refresh_rate); }
    void did_deliver_clock_tick(MonotonicTime frame_time);
    bool display_tick_requested() const { return m_rendering_opportunity_requested || m_clock_tick_requested; }

    void queue_present_frame(PendingFrame);
    Optional<Gfx::IntRect> pending_present_frame_viewport_rect() const;
    void mark_pending_present_frame_scheduled();
    void unschedule_pending_present_frame() { m_pending_present_frame_scheduled = false; }
    bool has_pending_present_frame_scheduled_on(Optional<u64> display_id) const;
    bool can_schedule_pending_present_frame_if_unblocked() const;
    Optional<PendingFrame> take_pending_present_frame_if_unblocked();
    bool needs_rasterization() const;
    Optional<Gfx::IntRect> frame_rect_to_repaint() const;
    Optional<Gfx::IntRect> self_present_rect() const;
    bool draws_canvas(Compositing::CanvasId) const;
    Optional<PreparedFrame> prepare_frame(Compositing::DisplayListPlayerSkia&, PendingFrame, CompositedContextResolver const*);
    void did_submit_prepared_frame(Gfx::IntRect);
    bool present_synchronously(Compositing::DisplayListPlayerSkia&, CompositedContextResolver const*);
    bool can_paint_screenshot(Gfx::ShareableBitmap&) const;
    void paint_screenshot(Compositing::DisplayListPlayerSkia&, Gfx::ShareableBitmap&, CompositedContextResolver const*);
    bool acknowledge_presented_bitmap(i32 bitmap_id);
    void did_finish_gpu_present(i32 bitmap_id);

private:
    struct ActiveSmoothScrollAnimation {
        Web::AsyncScrollNodeStableID stable_node_id;
        Compositing::AsyncScrollOperationID operation_id;
        Compositing::SmoothScrollAnimation animation;
        MonotonicTime started_at;
        bool is_user_scroll { false };
    };

    struct ActiveScrollFling {
        Compositing::ScrollFling fling;
        // The fling steps use the position and modifiers of the gesture end event.
        Web::DevicePixelPoint position;
        Web::DevicePixelPoint screen_position;
        Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
        bool stopped { false };
    };

    struct VisualViewportScrollDelta {
        Compositing::AsyncScrollOffset scroll_offset;
        Gfx::FloatPoint consumed_delta;
    };

    // The scroller the first step of a wheel gesture was routed to. Every later step of the gesture scrolls it without
    // hit testing, and stops at its edge rather than handing the rest of the gesture to an ancestor.
    struct WheelScrollLatch {
        Web::AsyncScrollNodeStableID stable_node_id;
        Compositing::WheelGestureIdentity gesture;
    };

    struct RasterizedFrame {
        NonnullRefPtr<Compositing::DisplayList const> display_list;
        Compositing::AccumulatedVisualContextTree visual_context_tree;
        Compositing::ScrollStateSnapshot scroll_state_snapshot;
        Gfx::IntSize viewport_size;
        HashMap<Compositing::CanvasId, u64> canvas_content_generations;
    };

    void stop_backing_store_shrink_timer();
    void stop_surplus_backing_store_retirement_timer();
    void end_keyboard_scroll_gesture();
    Compositing::ScrollStateSnapshot scroll_state_snapshot_at_keyboard_step_starts() const;
    void apply_keyboard_scroll_state(Compositing::KeyboardScrollState);
    Compositing::AccumulatedVisualContextTree const& current_visual_context_tree() const;
    Optional<float> visual_viewport_scale_for_compositing() const;
    Optional<VisualViewportScrollDelta> apply_visual_viewport_scroll_delta(Gfx::FloatPoint);
    Optional<Gfx::FloatPoint> reapply_unreconciled_async_scroll_offsets();
    void store_pending_async_scroll_offsets(ReadonlySpan<Compositing::AsyncScrollOffset>, Optional<Compositing::AsyncScrollOperationID> = {});
    struct WheelScrollOutcome {
        Optional<Compositing::AsyncScrollOperationID> operation_id;
        // The viewport to present, when the scroll moved a scrolling box or started a snap scroll of one.
        Optional<Gfx::IntRect> viewport_rect_to_present;
    };
    WheelScrollOutcome perform_wheel_scroll_of_node(Compositing::AsyncScrollNodeID, Gfx::FloatPoint delta, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, Compositing::AsyncScrollOperationTracking, Gfx::IntRect viewport_rect, MonotonicTime now, Compositing::ScrollChaining);
    // The latched scroller of the gesture the wheel event continues, in the current scroll tree, with the gesture
    // advanced to the event. A latch the event does not continue, or whose scroller the current display list no
    // longer has, is dropped here.
    Optional<Compositing::AsyncScrollNodeID> resolve_wheel_scroll_latch(Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    // The scroller the first step of a wheel gesture is routed to, which the gesture is latched to.
    Optional<Compositing::AsyncScrollNodeID> hit_test_and_latch_wheel_gesture(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now, Optional<Web::UniqueNodeID> expected_document_id);
    Gfx::IntRect note_async_scrolling_viewport_rect(Gfx::IntRect viewport_rect, Optional<Compositing::AsyncScrollOffset> const&);
    Optional<Compositing::AsyncScrollOperationID> snap_at_gesture_end(MonotonicTime now);
    Compositing::AsyncScrollOperationID start_snap_scroll(Compositing::AsyncScrollNodeID, ScrollSnapController::SnapScrollStart&&, bool settles_gesture, MonotonicTime now);
    void retire_smooth_scroll_animation(Web::AsyncScrollNodeStableID);
    void cancel_smooth_scroll_taken_over_by_user_input(Compositing::AsyncScrollNodeID);
    void track_wheel_event_for_scroll_fling(Web::MouseEvent const&, MonotonicTime now);
    ContextUpdateResult scroll_for_wheel_event(Web::MouseEvent const&, MonotonicTime now);
    Web::MouseEvent scroll_fling_event(Gfx::FloatPoint delta, Web::ScrollGesturePhase) const;
    void note_user_scroll_gesture_end_if_drag_ended(bool was_dragging_scrollbar);
    bool user_scroll_gesture_in_progress() const;
    void schedule_end_of_scroll_step_gestures(MonotonicTime now);
    Optional<PendingFrame> apply_scrollbar_drag(ScrollbarController::Drag const&);
    void rebuild_wheel_hit_test_targets();
    void discard_sampled_visual_context_tree();
    void invalidate_visual_context_tree_for_compositing();
    bool can_render_frame() const;
    Compositing::AccumulatedVisualContextTree const& visual_context_tree_for_compositing();
    enum class PaintUIOverlay : u8 {
        No,
        Yes,
    };
    void paint_current_display_list(Compositing::DisplayListPlayerSkia&, Gfx::PaintingSurface&, CompositedContextResolver const*, Optional<Gfx::IntRect> damage_rect = {}, PaintUIOverlay = PaintUIOverlay::Yes, bool apply_raster_transform = true);
    Gfx::IntSize raster_size() const;
    Gfx::IntRect raster_damage_rect(Gfx::IntRect) const;
    Gfx::IntRect frame_damage_for(PendingFrame const&);
    Gfx::IntRect damage_since_last_raster(Gfx::IntSize viewport_size);
    void remember_rasterized_frame(Gfx::IntSize viewport_size);
    void update_caret_blink_timer();
    void schedule_next_caret_blink();
    Gfx::IntRect caret_damage_rect();

    CompositorStateWebContentClient& m_web_content_client;
    Compositing::CanvasSurfaceRegistry const& m_canvas_surface_registry;
    Web::CompositorContextId m_context_id;
    Optional<u64> m_page_id;

    bool m_presents_to_client { false };
    Optional<Web::CompositorContextId> m_parent_context_id;

    RefPtr<Compositing::DisplayList const> m_display_list;
    Optional<Compositing::AccumulatedVisualContextTree> m_visual_context_tree;
    Optional<Compositing::AccumulatedVisualContextTree> m_visual_context_tree_for_compositing;
    Optional<Compositing::AccumulatedVisualContextTree> m_sampled_visual_context_tree;
    u64 m_visual_context_tree_copy_count { 0 };
    bool m_has_active_visual_animations { false };
    Optional<bool> m_animated_content_may_affect_viewport;
    Compositing::DisplayListResourceStorage m_display_list_resource_storage;
    Compositing::ScrollStateSnapshot m_scroll_state_snapshot;
    BackingStoreManager m_backing_store_manager;
    RefPtr<Gfx::PaintingSurface> m_latest_rendered_surface;
    RefPtr<Gfx::PaintingSurface> m_damage_surface;
    Optional<RasterizedFrame> m_last_rasterized_frame;
    Gfx::FloatSize m_raster_scale { 1, 1 };
    Gfx::FloatPoint m_raster_translation;

    Compositing::AsyncScrollTree m_async_scroll_tree;
    ScrollbarController m_scrollbar_controller;
    ScrollSnapController m_scroll_snap_controller;
    Vector<Compositing::StartedUserScroll> m_started_user_scrolls;

    Vector<Compositing::AsyncScrollOffset> m_pending_async_scroll_offsets;
    // Offsets handed to WebContent that no snapshot of its has incorporated yet; the compositor keeps
    // reapplying them over the main-thread state that arrives in the meantime.
    struct UnreconciledAsyncScrollOffset {
        u64 sequence { 0 };
        Web::AsyncScrollNodeStableID stable_node_id;
        Gfx::FloatPoint compositor_scroll_offset;
    };
    Vector<UnreconciledAsyncScrollOffset> m_unreconciled_async_scroll_offsets;
    u64 m_next_async_scroll_update_sequence { 0 };
    void retire_reconciled_async_scroll_offsets(u64 adopted_sequence);
    Vector<Compositing::AsyncScrollOperationID> m_completed_async_scroll_operation_ids;
    Vector<Compositing::AsyncScrollOperationID> m_async_scroll_operation_ids_taken_over_by_user_input;
    Compositing::KeyboardScrollState m_keyboard_scroll_state;
    Vector<Web::UIEvents::KeyCode, 3> m_held_scroll_keys;
    bool m_user_scroll_gesture_ended { false };
    bool m_published_user_scroll_gesture_in_progress { false };
    RefPtr<Core::Timer> m_scroll_step_gesture_input_timer;
    Optional<WheelScrollLatch> m_wheel_scroll_latch;
    Vector<ActiveSmoothScrollAnimation> m_smooth_scroll_animations;
    Compositing::ScrollVelocityTracker m_scroll_velocity_tracker;
    Compositing::ScrollFlingBooster m_scroll_fling_booster;
    Optional<ActiveScrollFling> m_scroll_fling;
    Compositing::AsyncScrollOperationID m_next_async_scroll_operation_id { 0 };
    Gfx::IntRect m_async_scrolling_viewport_rect;
    bool m_has_async_scrolling_state { false };
    bool m_can_accept_async_wheel_events { false };
    bool m_has_blocking_wheel_event_listeners { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    Compositing::WheelRoutingAdmission m_wheel_routing_admission { Compositing::WheelRoutingAdmission::NoAsyncScrollingState };
    Optional<Compositing::TransformWithOrigin> m_async_visual_viewport_transform;
    Optional<i64> m_visual_animation_sample_time_ns;

    Gfx::IntSize m_viewport_size;
    bool m_paused_debugger_overlay_visible { false };
    double m_paused_debugger_overlay_device_pixel_ratio { 1.0 };
    Optional<String> m_paused_debugger_overlay_font_family;
    Optional<Compositing::PausedDebuggerOverlayAction> m_paused_debugger_overlay_hovered_action;
    Compositing::WindowResizingInProgress m_window_resize_in_progress { Compositing::WindowResizingInProgress::No };
    RefPtr<Core::Timer> m_backing_store_shrink_timer;
    RefPtr<Core::Timer> m_surplus_backing_store_retirement_timer;
    Function<void(Gfx::IntRect)> m_schedule_caret_repaint;
    RefPtr<Core::Timer> m_caret_blink_timer;
    Optional<i64> m_caret_blink_cycle_start_time_ns;
    Optional<u64> m_display_id;
    double m_display_refresh_rate { 60.0 };
    Compositing::ContextVisibility m_visibility { Compositing::ContextVisibility::Visible };

    bool m_rendering_opportunity_requested { false };
    FramePacer m_rendering_opportunity_pacer;
    bool m_clock_tick_requested { false };
    FramePacer m_clock_tick_pacer;

    Optional<PendingFrame> m_pending_present_frame;
    bool m_pending_present_frame_scheduled { false };
    Optional<Gfx::IntRect> m_presented_frame;
};

}
