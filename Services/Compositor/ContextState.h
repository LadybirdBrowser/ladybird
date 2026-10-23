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
#include <Compositor/AsyncScrollUpdateOutbox.h>
#include <Compositor/CaretBlinkTimer.h>
#include <Compositor/CompositedContext.h>
#include <Compositor/ContextPresenter.h>
#include <Compositor/ContextScene.h>
#include <Compositor/ElasticOverscrollController.h>
#include <Compositor/Forward.h>
#include <Compositor/FramePacer.h>
#include <Compositor/KeyboardScrollController.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/ScrollFlingController.h>
#include <Compositor/ScrollSnapController.h>
#include <Compositor/ScrollbarController.h>
#include <Compositor/WheelGestureRouting.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Scrolling/SmoothScrollAnimation.h>
#include <LibCompositing/Types.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibGfx/Size.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/InputTrace.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Gfx {

class SkiaBackendContext;

}

namespace Compositor {

class CompositorStateWebContentClient;

class ContextState {
    AK_MAKE_NONCOPYABLE(ContextState);
    AK_MAKE_NONMOVABLE(ContextState);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct AsyncScrollResult {
        Compositing::AsyncScrollEnqueueResult enqueue_result;
        Optional<PendingFrame> frame_to_present {};

        static AsyncScrollResult declined(Web::InputRoutingDecision decision) { return { .enqueue_result = { .routing_decision = decision } }; }
    };

    // A wheel step that goes to a context nested in this one: the first step of a gesture over the content of the
    // nested context, or a step of a gesture latched to a context nested here.
    struct NestedContextWheelStep {
        Web::CompositorContextId context_id;
        // Where the first step lands in the content of the nested context.
        Optional<Compositing::NestedContextWheelHit> hit {};
    };

    struct ContextUpdateResult {
        bool accepted { false };
        Optional<PendingFrame> frame_to_present {};
        bool should_request_rendering_update { false };
        Optional<Web::ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor {};
        // Why a wheel or mouse event was taken or turned away, and the scroller it was routed to.
        Optional<Web::InputRoutingDecision> routing_decision {};
        Optional<Web::AsyncScrollNodeStableID> routed_scroller {};
        // How the first step of a wheel gesture went by a nested context, ahead of `routing_decision`.
        Optional<Web::InputRoutingDecision> nested_context_hand_off {};
        // Set instead of everything else for a wheel step that a nested context is to take.
        Optional<NestedContextWheelStep> nested_context_wheel_step {};

        static ContextUpdateResult declined(Web::InputRoutingDecision decision) { return { .routing_decision = decision }; }
    };

    // A step of a wheel gesture that the context this one is nested in hands to it, at a position in this context's
    // device pixels. The delta is in CSS pixels, with the axes already swapped for Shift.
    struct WheelStepFromContainingContext {
        Gfx::FloatPoint position;
        Gfx::FloatPoint delta_in_css_pixels;
        Web::WheelDeltaPrecision wheel_delta_precision { Web::WheelDeltaPrecision::Discrete };
        Web::ScrollGesturePhase scroll_gesture_phase { Web::ScrollGesturePhase::None };
        Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };

        static WheelStepFromContainingContext for_wheel_event(Web::MouseEvent const&);
    };

    // Where the first step of a gesture that the containing context hands in goes in this context. A nested context
    // passes a step on to the document around it unless one of its scrollers takes or keeps it.
    struct FirstWheelStepRoute {
        enum class Kind : u8 {
            Declined,
            ScrollNode,
            NestedContext,
            Kept,
            PassedOn,
        };
        Kind kind;
        Web::InputRoutingDecision decision;
        Optional<Compositing::AsyncScrollNodeID> node_id {};
        Optional<Web::AsyncScrollNodeStableID> scroller {};
        Optional<Compositing::NestedContextWheelHit> nested_context_hit {};
    };

    ContextState(Web::CompositorContextId, Optional<u64> page_id, CompositorStateWebContentClient&, OwnedCanvasSurfaces, Function<void(Gfx::IntRect)> schedule_caret_repaint = {}, Function<void()> request_animation_frames = {});
    ~ContextState();

    bool is_owned_by(CompositorStateWebContentClient const&) const;
    CompositorStateWebContentClient& web_content_client() const { return m_web_content_client; }
    void request_rendering_update();
    void dispatch_mouse_event_to_web_content(Web::MouseEvent const&, bool nested_context_scrolled_since_last_frame = false);
    void dispatch_key_event_to_web_content(Web::KeyEvent const&);
    // Whether the scroll offsets here moved past those of the last frame WebContent presented.
    bool scrolled_since_last_frame() const;
    bool can_dispatch_input_to_web_content() const { return m_page_id.has_value(); }
    Optional<u64> page_id() const { return m_page_id; }

    bool presents_to_client() const { return m_presenter.presents_to_client(); }
    void stop_presenting_to_client();

    void set_parent_context(Optional<Web::CompositorContextId>);
    Web::CompositorContextId context_id() const { return m_context_id; }
    Optional<Web::CompositorContextId> parent_context_id() const { return m_parent_context_id; }
    ContextPresenter& presenter() { return m_presenter; }
    ContextPresenter const& presenter() const { return m_presenter; }

    void apply_display_list_resource_transaction(Compositing::DisplayListResourceTransaction&&);
    void install_display_list_update(
        NonnullRefPtr<Compositing::DisplayList>,
        Compositing::AccumulatedVisualContextTree,
        Compositing::ScrollStateSnapshot&&);
    void update_visual_context_tree(Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void update_scroll_state(Compositing::ScrollStateSnapshot&&, Compositing::KeyboardScrollState);
    void set_video_sink(Compositing::VideoSinkResourceId, RefPtr<Media::DisplayingVideoSink>);
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_scene.video_sink_handles(); }

    void invalidate_wheel_event_listener_state(u64 generation);
    void invalidate_keyboard_scroll_state(u64 generation);
    ContextUpdateResult handle_key_event(Web::KeyEvent const&);
    ContextUpdateResult handle_mouse_event(Web::MouseEvent const&);
    // The UI's wheel deltas are in CSS pixels; this scales them for the scroll tree, which works in device pixels.
    ContextUpdateResult handle_wheel_event(Web::MouseEvent const&, Optional<MonotonicTime> now_for_testing = {});
    // Takes note of a wheel event from the UI as it arrives, ahead of anything that may turn it away or leave it to
    // the main thread.
    void note_wheel_event_from_ui(Web::MouseEvent const&, Optional<MonotonicTime> now_for_testing = {});
    ContextUpdateResult handle_pinch_event(Web::PinchEvent const&);

    bool may_hand_first_wheel_steps_to_nested_contexts() const;
    Optional<Compositing::NestedContextWheelHit> nested_context_for_wheel_at(Gfx::FloatPoint position);
    ContextUpdateResult scroll_for_wheel_event_passed_on_by_nested_context(Web::MouseEvent const&, MonotonicTime now);
    void latch_wheel_gesture_to_nested_context(Web::CompositorContextId, Optional<Web::AsyncScrollNodeStableID> scroller, Web::MouseEvent const&, MonotonicTime now);
    void forget_wheel_gesture_latch() { m_wheel_gesture_routing.clear_latch(); }

    FirstWheelStepRoute route_first_wheel_step_from_containing_context(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision, Compositing::NestedContextRegions);
    // Latches the gesture to the scroller the route found, or to the one that keeps the step, and scrolls by the step.
    ContextUpdateResult take_first_wheel_step_from_containing_context(FirstWheelStepRoute const&, WheelStepFromContainingContext const&, MonotonicTime now);
    ContextUpdateResult take_latched_wheel_step_from_containing_context(WheelStepFromContainingContext const&, MonotonicTime now);
    Optional<Web::UniqueNodeID> document_id() const { return m_async_scroll_tree.document_id(); }
    AsyncScrollResult async_scroll_by(
        Web::UniqueNodeID document_id,
        Gfx::FloatPoint position,
        Gfx::FloatPoint delta,
        Gfx::IntRect viewport_rect,
        Web::WheelDeltaPrecision,
        Web::ScrollGesturePhase,
        u32 modifiers,
        Compositing::AsyncScrollOperationTracking,
        Compositing::WheelStepDispatch,
        // The id the UI gave the wheel event, or 0 for a step that did not come from the UI.
        u64 input_event_id,
        Optional<MonotonicTime> now_for_testing = {});
    AsyncScrollResult smooth_scroll_to(Web::AsyncScrollNodeStableID, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind, Compositing::SmoothScrollInitiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue);
    void cancel_smooth_scroll(Web::AsyncScrollNodeStableID);
    Optional<Gfx::IntRect> advance_smooth_scroll_animations(MonotonicTime now);
    bool needs_animation_frames() { return has_active_smooth_scroll_animations() || has_active_scroll_fling() || m_elastic_overscroll_controller.has_rebound() || visual_animations_need_frame() || m_presenter.ui_overlay_fades_need_frame() || m_scrollbar_controller.fades_need_frame(MonotonicTime::now()); }

    bool has_active_scroll_fling() const { return m_scroll_fling_controller.is_active(); }
    // The wheel event of the next step of the fling, which is scrolled by like a wheel event from the UI.
    Optional<Web::MouseEvent> take_scroll_fling_step(MonotonicTime frame_time);
    Optional<Web::MouseEvent> end_scroll_fling();
    ContextUpdateResult scroll_for_fling_step(Web::MouseEvent const&, MonotonicTime now);
    void end_scroll_fling_on_next_step();
    // Starts a fling if the gesture that the event ends was a flick. Use only on platforms that send no momentum
    // scroll events.
    bool start_scroll_fling_if_flicked(Web::MouseEvent const& gesture_end_event, Optional<MonotonicTime> now_for_testing = {});
    // Ends the fling before a wheel event from a new gesture or other input.
    Optional<Web::MouseEvent> interrupt_scroll_fling(Web::MouseEvent const&, Optional<MonotonicTime> now_for_testing = {});
    void set_scrolls_elastically(bool);
    void set_scrollbar_track_press_jumps(bool jumps) { m_scrollbar_track_press_jumps = jumps; }
    Optional<Gfx::IntRect> advance_elastic_overscroll_rebounds(MonotonicTime frame_time);
    Optional<PendingFrame> advance_scrollbar_fades(MonotonicTime frame_time);
    // The installed scrolling state, its routing admission, and how far the given scroller is stretched, as JSON.
    String scroll_state_for_testing(Web::AsyncScrollNodeStableID stretched_scroller) const;
    Web::InputTraceRecorder& input_trace() { return m_input_trace; }
    // Where the first step of a wheel gesture at the position, by the delta in CSS pixels, would go.
    Compositing::FirstWheelStepRouteForTesting route_first_wheel_step_for_testing(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels, Compositing::NestedContextRegions);
    bool advance_visual_animations(MonotonicTime now);
    bool visual_animations_need_frame();
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates();
    bool has_pending_async_scroll_updates() const;

    void viewport_size_updated(Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    bool set_ui_overlays(Compositing::UIOverlays);

    bool set_display_metadata(Optional<u64> display_id, double refresh_rate);
    Optional<u64> display_id() const { return m_display_id; }
    double display_refresh_rate() const { return m_display_refresh_rate; }
    bool set_visibility(Compositing::ContextVisibility);
    Compositing::ContextVisibility visibility() const { return m_visibility; }

    bool request_rendering_opportunity(double maximum_frames_per_second) { return m_rendering_opportunity_request.request(maximum_frames_per_second); }
    bool rendering_opportunity_requested() const { return m_rendering_opportunity_request.is_requested(); }
    double rendering_opportunity_frame_interval(double display_refresh_rate) const { return m_rendering_opportunity_request.frame_interval(display_refresh_rate); }
    bool rendering_opportunity_is_due(MonotonicTime frame_time, double display_refresh_rate) const { return m_rendering_opportunity_request.is_due(frame_time, display_refresh_rate); }
    void did_deliver_rendering_opportunity(MonotonicTime frame_time) { m_rendering_opportunity_request.did_deliver(frame_time); }

    bool request_clock_tick(double maximum_frames_per_second) { return m_clock_tick_request.request(maximum_frames_per_second); }
    bool clock_tick_requested() const { return m_clock_tick_request.is_requested(); }
    double clock_tick_interval(double display_refresh_rate) const { return m_clock_tick_request.frame_interval(display_refresh_rate); }
    bool clock_tick_is_due(MonotonicTime frame_time, double display_refresh_rate) const { return m_clock_tick_request.is_due(frame_time, display_refresh_rate); }
    void did_deliver_clock_tick(MonotonicTime frame_time) { m_clock_tick_request.did_deliver(frame_time); }
    // Where the compositor has scrolled each scroll node to.
    Vector<Web::CompositorScrollOffset> scroll_offsets() const;
    bool display_tick_requested() const { return rendering_opportunity_requested() || clock_tick_requested(); }

    bool has_pending_present_frame_scheduled_on(Optional<u64> display_id) const;
    Optional<PreparedFrame> prepare_frame(DisplayListPlayerSkia&, PendingFrame, CompositedContextResolver const*);
    bool present_synchronously(DisplayListPlayerSkia&, CompositedContextResolver const*);
    bool can_paint_screenshot(Gfx::ShareableBitmap&) const;
    // Returns the scrollbar fade damage that the screenshot did not present on screen.
    Optional<PendingFrame> paint_screenshot(DisplayListPlayerSkia&, Gfx::ShareableBitmap&, CompositedContextResolver const*);

private:
    struct ActiveSmoothScrollAnimation {
        Web::AsyncScrollNodeStableID stable_node_id;
        Compositing::AsyncScrollOperationID operation_id;
        Compositing::SmoothScrollAnimation animation;
        MonotonicTime started_at;
        bool is_user_scroll { false };
    };

    struct VisualViewportScrollDelta {
        Compositing::AsyncScrollOffset scroll_offset;
        Gfx::FloatPoint consumed_delta;
    };

    void end_keyboard_scroll_gesture();
    Compositing::ScrollStateSnapshot scroll_state_snapshot_at_keyboard_step_starts() const;
    void adopt_keyboard_scroll_state(Compositing::KeyboardScrollState);
    Optional<VisualViewportScrollDelta> apply_visual_viewport_scroll_delta(Gfx::FloatPoint);
    // Pans the visual viewport of a zoomed page by as much of a delta in screen pixels as it has room for, and returns
    // that part of the delta.
    Gfx::FloatPoint pan_visual_viewport_by(Gfx::FloatPoint delta_in_screen_pixels);
    Optional<Gfx::FloatPoint> reapply_unreconciled_async_scroll_offsets();
    void store_pending_async_scroll_offsets(ReadonlySpan<Compositing::AsyncScrollOffset>, Optional<Compositing::AsyncScrollOperationID> = {});
    enum class VisualViewportPanning : u8 {
        // The step is in the pixels of the scroller, and only scrolls it.
        No,
        // The step is in screen pixels, and pans the visual viewport of a zoomed page before it scrolls anything.
        BeforeScrolling,
    };
    struct WheelScrollOutcome {
        Optional<Compositing::AsyncScrollOperationID> operation_id;
        // The viewport to present, when the step panned the visual viewport, moved a scrolling box or started a snap
        // scroll of one.
        Optional<Gfx::IntRect> viewport_rect_to_present;
        // What is presented differs only by an elastic overscroll, which WebContent has nothing to update for.
        bool only_elastic_overscroll_changed { false };
    };
    // A step of a wheel gesture that is latched to the node. A stretch of the node is relaxed before the visual
    // viewport is panned, the node only scrolls by what both leave of the step, and what the node cannot scroll by
    // either is what pulls it past its edge.
    WheelScrollOutcome perform_wheel_scroll_of_node(Compositing::AsyncScrollNodeID, Gfx::FloatPoint delta, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, Compositing::AsyncScrollOperationTracking, Gfx::IntRect viewport_rect, MonotonicTime now, Compositing::ScrollChaining, VisualViewportPanning);
    struct NodeScrollByWheelDelta {
        WheelScrollOutcome outcome;
        // Empty when a wheel animation or a snap scroll took the delta, and when it scrolled an ancestor of the node.
        Optional<Gfx::FloatPoint> delta_the_node_did_not_scroll_by;
    };
    NodeScrollByWheelDelta scroll_node_by_wheel_delta(Compositing::AsyncScrollNodeID, Gfx::FloatPoint delta, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, Compositing::AsyncScrollOperationTracking, Gfx::IntRect viewport_rect, MonotonicTime now, Compositing::ScrollChaining);
    WheelGestureRouting::ResolvedLatch resolve_wheel_scroll_latch(Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    struct WheelGestureLatchAttempt {
        Web::InputRoutingDecision decision;
        Optional<Compositing::AsyncScrollNodeID> latched_node_id {};
    };
    // The scroller the first step of a wheel gesture is routed to, which the gesture is latched to.
    WheelGestureLatchAttempt hit_test_and_latch_wheel_gesture(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::WheelDeltaPrecision, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now, Optional<Web::UniqueNodeID> expected_document_id, Compositing::WheelStepDispatch, Compositing::NestedContextRegions);
    ContextUpdateResult take_wheel_step_from_containing_context(Compositing::AsyncScrollNodeID, Web::InputRoutingDecision, WheelStepFromContainingContext const&, Compositing::ScrollChaining, MonotonicTime now);
    Gfx::IntRect note_async_scrolling_viewport_rect(Gfx::IntRect viewport_rect, Optional<Compositing::AsyncScrollOffset> const&);
    Optional<Compositing::AsyncScrollOperationID> snap_at_gesture_end(MonotonicTime now);
    Compositing::AsyncScrollOperationID start_snap_scroll(Compositing::AsyncScrollNodeID, ScrollSnapController::SnapScrollStart&&, bool settles_gesture, MonotonicTime now);
    bool has_active_smooth_scroll_animations() const { return !m_smooth_scroll_animations.is_empty(); }
    void retire_smooth_scroll_animation(Web::AsyncScrollNodeStableID);
    void cancel_smooth_scroll_taken_over_by_user_input(Compositing::AsyncScrollNodeID);
    bool wheel_step_can_stretch_elastically(Web::WheelDeltaPrecision, Web::ScrollGesturePhase) const;
    bool pull_elastic_overscroll_by(Compositing::AsyncScrollNode const&, Gfx::FloatPoint step_delta, Gfx::FloatPoint delta_the_scroller_did_not_scroll_by);
    Optional<Compositing::ScrollStateSnapshot> scroll_state_snapshot_stretched_by_elastic_overscrolls() const;
    ContextUpdateResult scroll_for_wheel_event(Web::MouseEvent const&, MonotonicTime now);
    ContextUpdateResult scroll_for_wheel_event_in_this_context(Web::MouseEvent const&, Optional<Compositing::AsyncScrollNodeID> latched_node_id, Compositing::NestedContextRegions, MonotonicTime now);
    Gfx::FloatPoint wheel_event_delta_in_device_pixels(Web::MouseEvent const&) const;
    bool step_continues_touchpad_gesture_the_page_let_through(Optional<Compositing::AsyncScrollNodeID> const& latched_node_id, Web::WheelDeltaPrecision, Web::ScrollGesturePhase) const;
    Optional<Gfx::FloatPoint> in_flight_wheel_destination(Web::AsyncScrollNodeStableID) const;
    AsyncScrollResult smooth_wheel_scroll_by(Optional<Compositing::WheelScrollTarget> const&, Gfx::IntRect viewport_rect, MonotonicTime now);
    void retarget_user_scroll(ActiveSmoothScrollAnimation&, Gfx::FloatPoint destination_offset, MonotonicTime now);
    void report_started_user_scroll(Compositing::AsyncScrollOperationID, Web::AsyncScrollNodeStableID, Gfx::FloatPoint step_start_offset, Gfx::FloatPoint destination_offset, Compositing::ScrollAnimationKind, bool is_absolute_scroll = false);
    void note_user_scroll_gesture_end_if_drag_ended(bool was_dragging_scrollbar);
    bool user_scroll_gesture_in_progress() const;
    void schedule_end_of_scroll_step_gestures(MonotonicTime now);
    // A gesture whose steps this context chains ends once they stop arriving, which is reported to WebContent so
    // that it settles the gesture then rather than on a clock of its own.
    void end_scroll_step_gestures_whose_input_ran_out(MonotonicTime now);
    Optional<PendingFrame> apply_scrollbar_drag(ScrollbarController::Drag const&);
    Optional<PendingFrame> start_scrollbar_press_scroll(Compositing::AsyncScrollNode const&, Gfx::FloatPoint old_scroll_offset, Gfx::FloatPoint destination, bool is_absolute_scroll);
    Optional<PendingFrame> page_captured_scrollbar();
    void stop_scrollbar_paging();
    // The smooth scroll that a press beside the thumb started, while it lasts.
    ActiveSmoothScrollAnimation* running_scrollbar_press_scroll();
    void rebuild_wheel_hit_test_targets();
    ContextPresenter::FrameContent frame_content(Compositing::ScrollStateSnapshot const& presented_scroll_state_snapshot, CompositedContextResolver const*) const;
    Gfx::IntRect damage_rect_of_blinking_carets();
    Gfx::IntRect update_scrollbar_visibility();
    void report_scrollbar_visibility_changes();

    CompositorStateWebContentClient& m_web_content_client;
    Web::CompositorContextId m_context_id;
    Optional<u64> m_page_id;

    Optional<Web::CompositorContextId> m_parent_context_id;

    ContextScene m_scene;
    ContextPresenter m_presenter;
    Compositing::ScrollStateSnapshot m_scroll_state_snapshot;

    Compositing::AsyncScrollTree m_async_scroll_tree;
    ScrollbarController m_scrollbar_controller;
    ScrollSnapController m_scroll_snap_controller;

    AsyncScrollUpdateOutbox m_async_scroll_update_outbox;
    KeyboardScrollController m_keyboard_scroll_controller;
    RefPtr<Core::Timer> m_scroll_step_gesture_input_timer;
    WheelGestureRouting m_wheel_gesture_routing;
    Vector<ActiveSmoothScrollAnimation> m_smooth_scroll_animations;
    // The smooth scroll that a press beside the thumb of the captured scrollbar started.
    Optional<Compositing::AsyncScrollOperationID> m_scrollbar_press_scroll_operation;
    // A press beside the thumb that pages toward the pointer for as long as it is held.
    struct ScrollbarPaging {
        int direction { 0 };
        float primary_position { 0 };
    };
    Optional<ScrollbarPaging> m_scrollbar_paging;
    bool m_scrollbar_track_press_jumps { false };
    RefPtr<Core::Timer> m_scrollbar_paging_timer;
    ScrollFlingController m_scroll_fling_controller;
    ElasticOverscrollController m_elastic_overscroll_controller;
    Gfx::IntRect m_async_scrolling_viewport_rect;
    bool m_has_async_scrolling_state { false };

    CaretBlinkTimer m_caret_blink_timer;
    Function<void()> m_request_animation_frames;
    RefPtr<Core::Timer> m_scrollbar_fade_out_timer;
    Web::InputTraceRecorder m_input_trace;
    Optional<u64> m_display_id;
    double m_display_refresh_rate { 60.0 };
    Compositing::ContextVisibility m_visibility { Compositing::ContextVisibility::Visible };

    PacedTickRequest m_rendering_opportunity_request;
    PacedTickRequest m_clock_tick_request;
};

}
