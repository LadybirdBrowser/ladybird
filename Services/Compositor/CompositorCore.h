/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreHost.h>
#include <Compositor/ContextState.h>

namespace Compositor {

class CompositorCore {
    AK_MAKE_NONCOPYABLE(CompositorCore);
    AK_MAKE_NONMOVABLE(CompositorCore);

public:
    explicit CompositorCore(CompositorCoreHost&);

    enum class ContextOwnerCheckResult : u8 {
        OwnedByClient,
        ContextUnavailable,
        ConflictingOwner,
    };

    void create_context(Web::CompositorContextId, Optional<u64> page_id, WebContentClientId);
    // Also leaves the contexts nested in it without a parent.
    void destroy_context(Web::CompositorContextId);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId) const;
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);
    // Ends the fling of a context that is hidden, whether or not it was visible.
    void set_visibility(Web::CompositorContextId, Compositing::ContextVisibility, MonotonicTime now);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize);
    void set_paused_debugger_overlay_is_shown(Web::CompositorContextId, bool);
    void set_bound_presentation(Web::CompositorContextId, Optional<PresentationId>);
    void set_scroll_capabilities(Web::CompositorContextId, ContextState::ScrollCapabilities const&);

    void install_display_list_update(Web::CompositorContextId, NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree, Compositing::ScrollStateSnapshot&&, MonotonicTime now);
    // Returns false, leaving everything as it was, for a tree that does not fit the installed display list.
    bool update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree, MonotonicTime now);

    bool needs_animation_frames(Web::CompositorContextId, MonotonicTime now);
    Optional<FrameInputs> frame_inputs(Web::CompositorContextId, MonotonicTime now);
    ContextState::ScreenshotInputs frame_inputs_for_screenshot(Web::CompositorContextId, MonotonicTime now);
    DisplayTickResult display_tick(Web::CompositorContextId, MonotonicTime frame_time, MonotonicTime now, DisplayTickInputs const&);
    void timer_fired(Web::CompositorContextId, ContextTimer, MonotonicTime now);

    // The frames asked for since the last call, in the order they were asked for.
    Vector<PresentRequest> take_present_requests();
    void handle_and_dispatch_key_event(Web::CompositorContextId context_id, Web::KeyEvent event, bool allow_keyboard_scroll, MonotonicTime now);
    void handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event, MonotonicTime now);
    void handle_and_dispatch_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent event);
    Compositing::WheelEventAnswer take_wheel_event_from_main_thread(Web::CompositorContextId context_id, Web::MouseEvent const& event, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelEventRouting routing, MonotonicTime now);
    Compositing::WheelEventAnswer scroll_for_wheel_event_the_page_let_through(Web::CompositorContextId listener_context_id, Compositing::WheelStepLetThrough const& let_through, Compositing::AsyncScrollOperationTracking operation_tracking, WebContentClientId page_client, MonotonicTime now);
    void forget_wheel_gesture_the_page_canceled(Web::CompositorContextId listener_context_id, u64 gesture_id);
    void apply_scroll_requests(Web::CompositorContextId context_id, ReadonlySpan<Compositing::ScrollRequest> scroll_requests, MonotonicTime now);
    Compositing::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::CompositorContextId context_id);
    void invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation);
    void invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation);
    void update_scroll_state(Web::CompositorContextId context_id, Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state);
    String scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller) const;
    Optional<Compositing::FirstWheelStepRouteForTesting> route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels);
    void begin_input_trace_for_testing(Web::CompositorContextId context_id);
    void clear_per_test_input_state_for_testing(Web::CompositorContextId context_id);
    Vector<Web::InputTraceRecord> take_input_trace_for_testing(Web::CompositorContextId context_id);

private:
    template<typename Message, typename... Arguments>
    void post_to_presentation(PresentationId presentation_id, Arguments&&... arguments)
    {
        auto buffer = MUST(Message::static_encode(forward<Arguments>(arguments)...));
        m_host.post_message_to_presentation(presentation_id, buffer);
    }

    bool apply_context_update_result(
        Web::CompositorContextId,
        ContextState&,
        ContextState::ContextUpdateResult const&);
    enum class WheelStepOrigin : u8 {
        UI,
        Fling,
        MainThread,
    };
    // A fling step is scrolled at the frame time the fling synthesized it for.
    ContextState::ContextUpdateResult scroll_for_wheel_step(Web::CompositorContextId, ContextState&, Web::MouseEvent const&, WheelStepOrigin, bool is_first_wheel_event_with_delta, Compositing::AsyncScrollOperationTracking, MonotonicTime now);
    // How a wheel step went in a context that presented and published what the step changed itself.
    struct AppliedWheelStep {
        // What is left for the context that routes the step to present and report.
        ContextState::ContextUpdateResult result;
        bool presents_frame { false };
    };
    // A process only knows the ids of its own nodes, and only follows the operations of its own contexts.
    AppliedWheelStep apply_wheel_step_result(Web::CompositorContextId, ContextState&, ContextState::ContextUpdateResult const&, WebContentClientId reporting_client, MonotonicTime now);
    static Compositing::WheelEventAnswer wheel_event_answer_for(AppliedWheelStep const&, Optional<Web::WheelGestureMark>);
    AppliedWheelStep hand_wheel_step_to_nested_context(Web::CompositorContextId, ContextState&, Web::MouseEvent const&, ContextState::NestedContextWheelStep const&, ContextState::WheelEventListenerGate const&, Compositing::AsyncScrollOperationTracking, MonotonicTime now);
    struct NestedContextTakingWheelStep {
        Web::CompositorContextId context_id;
        // Null when the step cannot be handed to the context.
        ContextState* context { nullptr };
        Gfx::FloatPoint position {};
        ContextState::FirstWheelStepRoute route;
    };
    struct FirstWheelStepInNestedContexts {
        // The innermost context that the step went into, whose page sees the step.
        Web::CompositorContextId innermost_context_id;
        // The context that takes the step, which is empty when each of them passes the step on.
        Optional<NestedContextTakingWheelStep> taking_context {};
    };
    // Where the first step of a gesture over the content of a context nested in the context `context_id` goes, among
    // that context and the contexts nested in it.
    FirstWheelStepInNestedContexts first_wheel_step_in_nested_contexts(Web::CompositorContextId, Compositing::NestedContextHit, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision);
    // Routes the first step of a gesture that the page of the listener context let through: in that context, from the
    // scrollers the page found around the target, and on to the contexts around it for what a context passes on, up to
    // the routing context, which latches the gesture. A page of another process than a context's scrolls it only by
    // what the UI left to the page in the gesture.
    AppliedWheelStep route_wheel_step_the_page_let_through(Web::CompositorContextId routing_context_id, ContextState& routing_context, Web::CompositorContextId listener_context_id, ContextState& listener_context, ContextState::WheelStep, ReadonlySpan<Web::AsyncScrollNodeStableID> scroll_chain, Optional<WebContentClientId> page_client, MonotonicTime now);
    // The nearest context, from the given one out to the presenting one, that noted the gesture.
    Optional<Web::CompositorContextId> routing_context_of_wheel_gesture(Web::CompositorContextId, u64 gesture_id);
    // The scrollbars of a context nested in the presenting context `context_id` take the pointer over that context's
    // content, and a drag of one of them takes the pointer wherever it goes until the drag ends.
    ContextState::ContextUpdateResult handle_mouse_event_in_nested_contexts(Web::CompositorContextId, ContextState&, Web::MouseEvent const&, MonotonicTime now);
    ContextState* context_still_nested_in(Web::CompositorContextId containing_context_id, Optional<Web::CompositorContextId>);
    // Where the position in the presenting context lands in the nested context, through each context in between and
    // without regard to where their content is or what clips it.
    Optional<Gfx::FloatPoint> position_in_nested_context_ignoring_clips(Web::CompositorContextId presenting_context_id, ContextState& presenting_context, ContextState& nested_context, Gfx::FloatPoint position);
    bool is_nested_in(Web::CompositorContextId containing_context_id, ContextState const&) const;
    bool page_under_pinch_may_cancel_it(Web::CompositorContextId, ContextState&, Gfx::FloatPoint position);
    void dispatch_scroll_fling_step(Web::CompositorContextId, ContextState&, Optional<Web::MouseEvent>, MonotonicTime now);
    ContextState* context_if_present(Web::CompositorContextId);
    // Hands the context's async scroll updates to its WebContent process as soon as they exist,
    // so a rendering update reads them locally instead of asking for them over a synchronous call.
    void publish_pending_async_scroll_updates(Web::CompositorContextId, ContextState&);

    ContextState const* context_if_present(Web::CompositorContextId) const;
    bool nested_context_scrolled_since_last_frame(Web::CompositorContextId) const;
    void request_present(Web::CompositorContextId, PendingFrame const&);

    CompositorCoreHost& m_host;
    HashMap<Web::CompositorContextId, OwnPtr<ContextState>> m_contexts;
    Vector<PresentRequest> m_present_requests;
    mutable bool m_is_entered { false };
};

}
