/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <Compositor/CompositorCore.h>
#include <Compositor/CompositorPresentationClientEndpoint.h>

namespace Compositor {

CompositorCore::CompositorCore(CompositorCoreHost& host)
    : m_host(host)
{
}

void CompositorCore::create_context(Web::CompositorContextId context_id, Optional<u64> page_id, WebContentClientId web_content_client_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    VERIFY(!m_contexts.contains(context_id));
    if (page_id.has_value())
        VERIFY(context_id == Web::compositor_context_id_for_page(*page_id));

    m_contexts.ensure(context_id, [&] {
        return make<ContextState>(context_id, page_id, web_content_client_id, m_host);
    });
}

void CompositorCore::destroy_context(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    VERIFY(context_if_present(context_id));
    for (auto& context_entry : m_contexts) {
        if (context_entry.key == context_id)
            continue;
        auto& possible_child_context = *context_entry.value;
        auto parent_context_id = possible_child_context.parent_context_id();
        if (parent_context_id.has_value() && *parent_context_id == context_id)
            possible_child_context.set_parent_context({});
    }
    m_contexts.remove(context_id);
}

CompositorCore::ContextOwnerCheckResult CompositorCore::check_context_owner(Web::CompositorContextId context_id, WebContentClientId client) const
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto const* context = context_if_present(context_id);
    if (!context)
        return ContextOwnerCheckResult::ContextUnavailable;
    if (!context->is_owned_by(client))
        return ContextOwnerCheckResult::ConflictingOwner;

    return ContextOwnerCheckResult::OwnedByClient;
}

void CompositorCore::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->set_parent_context(parent_context_id);
}

void CompositorCore::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->stop_presenting_to_client();
}

void CompositorCore::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return;
    context->viewport_size_updated(viewport_size);
}

void CompositorCore::set_paused_debugger_overlay_is_shown(Web::CompositorContextId context_id, bool is_shown)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return;
    context->set_paused_debugger_overlay_is_shown(is_shown);
}

void CompositorCore::set_bound_presentation(Web::CompositorContextId context_id, Optional<PresentationId> presentation_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->set_bound_presentation(presentation_id);
}

void CompositorCore::set_scroll_capabilities(Web::CompositorContextId context_id, ContextState::ScrollCapabilities const& capabilities)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->set_scroll_capabilities(capabilities);
}

void CompositorCore::install_display_list_update(Web::CompositorContextId context_id, NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::ScrollStateSnapshot&& scroll_state_snapshot, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->install_display_list_update(move(display_list), move(visual_context_tree), move(scroll_state_snapshot), now);
}

bool CompositorCore::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    return context->update_visual_context_tree(move(visual_context_tree), now);
}

bool CompositorCore::needs_animation_frames(Web::CompositorContextId context_id, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    return context->needs_animation_frames(now);
}

Optional<FrameInputs> CompositorCore::frame_inputs(Web::CompositorContextId context_id, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    return context->frame_inputs(now);
}

ContextState::ScreenshotInputs CompositorCore::frame_inputs_for_screenshot(Web::CompositorContextId context_id, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    return context->frame_inputs_for_screenshot(now);
}

void CompositorCore::set_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return;
    if (visibility == Compositing::ContextVisibility::Hidden)
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(), now);
    context->set_visibility(visibility);
}

void CompositorCore::timer_fired(Web::CompositorContextId context_id, ContextTimer timer, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return;
    auto caret_damage_rect = context->timer_fired(timer, now);
    if (caret_damage_rect.has_value())
        m_present_requests.append({ context_id, {}, *caret_damage_rect });
}

Vector<PresentRequest> CompositorCore::take_present_requests()
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    return move(m_present_requests);
}

void CompositorCore::update_scroll_state(Web::CompositorContextId context_id, Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->update_scroll_state(move(scroll_state_snapshot), move(keyboard_scroll_state));
}

void CompositorCore::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->invalidate_wheel_event_listener_state(generation);
}

void CompositorCore::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    if (auto* context = context_if_present(context_id))
        context->invalidate_keyboard_scroll_state(generation);
}

void CompositorCore::handle_and_dispatch_key_event(Web::CompositorContextId context_id, Web::KeyEvent event, bool allow_keyboard_scroll, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    // NB: The presentation connection only passes on input for the contexts bound to it.
    auto* context = context_if_present(context_id);
    VERIFY(context);
    VERIFY(context->bound_presentation().has_value());
    auto presentation_id = *context->bound_presentation();
    if (!context->can_dispatch_input_to_web_content()) {
        // No context could take the event; the UI sends it to WebContent itself.
        post_to_presentation<Messages::CompositorPresentationClient::DidNotDispatchInputEvent>(presentation_id, context_id, event.id);
        return;
    }

    bool later_keys_need_not_wait = false;
    if (allow_keyboard_scroll) {
        if (auto box = context->box_scrolled_for_key_event(event); box.has_value())
            dispatch_scroll_fling_step(context_id, *context, context->end_momentum_scrolling(box.value()), now);
        auto result = context->handle_key_event(event, now);
        later_keys_need_not_wait = apply_context_update_result(context_id, *context, result) && event.type == Web::KeyEvent::Type::KeyDown;
        event.async_scroll_performed_default_action = later_keys_need_not_wait && !result.key_found_nothing_to_scroll;
        if (later_keys_need_not_wait && result.key_found_nothing_to_scroll)
            event.user_input_scroll_count_with_nothing_to_scroll = context->user_input_scroll_count();
    }

    // The scroll updates the event produced reach WebContent ahead of the event itself on the same connection.
    publish_pending_async_scroll_updates(context_id, *context);
    context->dispatch_key_event_to_web_content(event);
    // The compositor forwarded the key event to WebContent, after performing its default action by scrolling if it says so.
    post_to_presentation<Messages::CompositorPresentationClient::DidDispatchInputEvent>(presentation_id, context_id, event.id, later_keys_need_not_wait);
}

void CompositorCore::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    // NB: The presentation connection only passes on input for the contexts bound to it.
    auto* context = context_if_present(context_id);
    VERIFY(context);
    VERIFY(context->bound_presentation().has_value());
    auto presentation_id = *context->bound_presentation();
    auto record_routing = [&](Web::InputRoutingDecision decision, Optional<Web::AsyncScrollNodeStableID> scroller = {}) {
        context->input_trace().record({ .input_event_id = event.id, .stage = Web::InputTraceStage::Compositor, .decision = decision, .scroller = scroller });
    };
    if (!context->can_dispatch_input_to_web_content()) {
        record_routing(Web::InputRoutingDecision::NotDispatchedByCompositor);
        post_to_presentation<Messages::CompositorPresentationClient::DidNotDispatchInputEvent>(presentation_id, context_id, event.id);
        return;
    }

    if (event.type == Web::MouseEvent::Type::MouseMove)
        context->note_pointer_position(Gfx::FloatPoint { static_cast<float>(event.position.x().value()), static_cast<float>(event.position.y().value()) });
    else if (first_is_one_of(event.type, Web::MouseEvent::Type::MouseLeave, Web::MouseEvent::Type::MouseCancel))
        context->note_pointer_position({});

    if (event.type == Web::MouseEvent::Type::MouseDown)
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(), now);

    auto is_wheel_event = event.type == Web::MouseEvent::Type::MouseWheel;
    bool is_first_wheel_event_with_delta = false;
    if (is_wheel_event) {
        // A key, or a script's smooth scroll, takes over from momentum, until wheel input other than the momentum takes
        // over again.
        if (context->absorbs_wheel_event_of_interrupted_momentum(event)) {
            record_routing(Web::InputRoutingDecision::MomentumInterrupted);
            // The compositor performed the event's whole default action, so the UI hears nothing more about it.
            post_to_presentation<Messages::CompositorPresentationClient::DidConsumeInputEvent>(presentation_id, context_id, event.id);
            return;
        }
        auto noted = context->note_wheel_event_from_ui(event, now);
        event.wheel_gesture_mark = noted.wheel_gesture_mark;
        is_first_wheel_event_with_delta = noted.is_first_wheel_event_with_delta;

        // The UI can send the end of a gesture more than once. An end that arrives during a fling belongs to the
        // flicked gesture, so it does not stop the fling.
        if (event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->has_active_scroll_fling()) {
            record_routing(Web::InputRoutingDecision::GestureEndAbsorbedByFling);
            post_to_presentation<Messages::CompositorPresentationClient::DidConsumeInputEvent>(presentation_id, context_id, event.id);
            return;
        }

        // A new gesture must not continue the gesture of the fling, so the fling ends first.
        dispatch_scroll_fling_step(context_id, *context, context->interrupt_scroll_fling(event, now), now);

        // If the gesture continues as a fling, the end of the fling is the end of the gesture.
        if (context->presentation_synthesizes_scroll_momentum() && event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->start_scroll_fling_if_flicked(event, now)) {
            record_routing(Web::InputRoutingDecision::FlingStarted);
            post_to_presentation<Messages::CompositorPresentationClient::DidConsumeInputEvent>(presentation_id, context_id, event.id);
            context->request_display_ticks_if_needed(now);
            return;
        }
    }

    ContextState::ContextUpdateResult result;
    if (is_wheel_event)
        result = scroll_for_wheel_step(context_id, *context, event, WheelStepOrigin::UI, is_first_wheel_event_with_delta, Compositing::AsyncScrollOperationTracking::No, now);
    else
        result = handle_mouse_event_in_nested_contexts(context_id, *context, event, now);
    if (result.nested_context_hand_off.has_value())
        record_routing(*result.nested_context_hand_off);
    if (result.routing_decision.has_value()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} routed input event {}: {}", context_id, event.id, Web::input_routing_decision_name(*result.routing_decision));
        record_routing(*result.routing_decision, result.routed_scroller);
    }

    // Asks for the present, publishes the scroll updates the event produced and asks for a rendering update, the last
    // two of which reach WebContent ahead of the event itself on the same connection. Those of a nested context
    // another process owns go on that process's connection instead, while the event reaches it through the UI.
    auto handled = apply_context_update_result(context_id, *context, result);
    // A wheel event that lets go of a stretched scroller presents nothing itself; the rebound does from here on. A press
    // beside the thumb of a scrollbar scrolls there in an animation too.
    context->request_display_ticks_if_needed(now);
    if (is_wheel_event) {
        event.async_scroll_performed_default_action = handled;
        if (!handled)
            context->note_wheel_event_left_to_the_page(event);
    } else if (handled) {
        // The page still sees the events of a drag the compositor scrolls for a scrollbar the display list paints.
        if (!result.scrollbar_dragged_by_compositor.has_value()) {
            record_routing(Web::InputRoutingDecision::ConsumedByCompositor);
            post_to_presentation<Messages::CompositorPresentationClient::DidConsumeInputEvent>(presentation_id, context_id, event.id);
            return;
        }
        event.scrollbar_dragged_by_compositor = result.scrollbar_dragged_by_compositor;
    }
    record_routing(Web::InputRoutingDecision::ForwardedToPage);
    context->dispatch_mouse_event_to_web_content(event, nested_context_scrolled_since_last_frame(context_id));
}

// The content box of a nested context is drawn scaled to the size of its viewport.
static Optional<Gfx::FloatPoint> position_in_nested_context(Compositing::NestedContextHit const& hit, ContextState const& nested_context)
{
    auto viewport_size = nested_context.viewport_size().to_type<float>();
    if (hit.content_box_size.is_empty() || viewport_size.is_empty())
        return {};
    return Gfx::FloatPoint {
        hit.position_in_content_box.x() * viewport_size.width() / hit.content_box_size.width(),
        hit.position_in_content_box.y() * viewport_size.height() / hit.content_box_size.height(),
    };
}

ContextState* CompositorCore::context_still_nested_in(Web::CompositorContextId containing_context_id, Optional<Web::CompositorContextId> context_id)
{
    if (!context_id.has_value())
        return nullptr;
    auto* context = context_if_present(*context_id);
    if (!context || !is_nested_in(containing_context_id, *context))
        return nullptr;
    return context;
}

Optional<Gfx::FloatPoint> CompositorCore::position_in_nested_context_ignoring_clips(Web::CompositorContextId presenting_context_id, ContextState& presenting_context, ContextState& nested_context, Gfx::FloatPoint position)
{
    Vector<ContextState*, 4> contexts_from_nested_context_outward;
    auto* context = &nested_context;
    // A walk up the parents that outlasts the number of contexts went around a loop.
    for (size_t remaining_contexts = m_contexts.size(); context != &presenting_context; --remaining_contexts) {
        if (remaining_contexts == 0)
            return {};
        contexts_from_nested_context_outward.append(context);
        auto parent_context_id = context->parent_context_id();
        if (!parent_context_id.has_value())
            return {};
        context = *parent_context_id == presenting_context_id ? &presenting_context : context_if_present(*parent_context_id);
        if (!context)
            return {};
    }

    auto* containing_context = &presenting_context;
    for (auto* context_in_between : contexts_from_nested_context_outward.in_reverse()) {
        auto hit = containing_context->position_in_content_of_nested_context(context_in_between->context_id(), position);
        if (!hit.has_value())
            return {};
        auto position_in_context = position_in_nested_context(*hit, *context_in_between);
        if (!position_in_context.has_value())
            return {};
        position = *position_in_context;
        containing_context = context_in_between;
    }
    return position;
}

ContextState::ContextUpdateResult CompositorCore::handle_mouse_event_in_nested_contexts(Web::CompositorContextId context_id, ContextState& context, Web::MouseEvent const& event, MonotonicTime now)
{
    using Type = Web::MouseEvent::Type;
    auto& pointer = context.pointer_in_nested_contexts();

    auto* context_with_captured_scrollbar = context_still_nested_in(context_id, pointer.context_with_captured_scrollbar);
    if (!context_with_captured_scrollbar || !context_with_captured_scrollbar->has_captured_scrollbar()) {
        pointer.context_with_captured_scrollbar.clear();
        context_with_captured_scrollbar = nullptr;
    }

    // The nested context presents and publishes what the event changed, and this context reports how the event went.
    auto result_of_nested_context = [&](Web::CompositorContextId nested_context_id, ContextState& nested_context, ContextState::ContextUpdateResult const& nested_result, Optional<PendingFrame> frame_of_this_context) {
        apply_context_update_result(nested_context_id, nested_context, nested_result);
        nested_context.request_display_ticks_if_needed(now);
        // Node ids are only unique within the process that made them.
        bool ids_are_of_this_process = nested_context.is_owned_by(context.web_content_client_id());
        return ContextState::ContextUpdateResult {
            .accepted = nested_result.accepted,
            .frame_to_present = move(frame_of_this_context),
            .scrollbar_dragged_by_compositor = nested_result.scrollbar_dragged_by_compositor,
            .routing_decision = nested_result.routing_decision,
            .routed_scroller = ids_are_of_this_process ? nested_result.routed_scroller : Optional<Web::AsyncScrollNodeStableID> {},
            .nested_context_hand_off = Web::InputRoutingDecision::HandedToNestedContext,
        };
    };

    auto send_to_context_with_captured_scrollbar = [&](Web::MouseEvent const& event_to_send) {
        auto nested_context_id = *pointer.context_with_captured_scrollbar;
        auto position = Gfx::FloatPoint {
            static_cast<float>(event_to_send.position.x().value()),
            static_cast<float>(event_to_send.position.y().value()),
        };
        // A leave or a cancel has no position of its own.
        if (first_is_one_of(event_to_send.type, Type::MouseDown, Type::MouseMove, Type::MouseUp)) {
            if (auto position_in_nested_context = position_in_nested_context_ignoring_clips(context_id, context, *context_with_captured_scrollbar, position); position_in_nested_context.has_value())
                pointer.last_position_in_context_with_captured_scrollbar = *position_in_nested_context;
        }
        auto nested_result = context_with_captured_scrollbar->handle_mouse_event_from_containing_context(event_to_send, pointer.last_position_in_context_with_captured_scrollbar, now);
        auto result = result_of_nested_context(nested_context_id, *context_with_captured_scrollbar, nested_result, {});
        if (!context_with_captured_scrollbar->has_captured_scrollbar()) {
            pointer.context_with_captured_scrollbar.clear();
            context_with_captured_scrollbar = nullptr;
            pointer.context_under_pointer = nested_context_id;
        }
        return result;
    };

    // The scrollbar the pointer was over in the nested context it left is no longer hovered.
    auto note_context_under_pointer = [&](Optional<Web::CompositorContextId> nested_context_id) {
        if (pointer.context_under_pointer == nested_context_id)
            return;
        if (auto* context_left = context_still_nested_in(context_id, pointer.context_under_pointer)) {
            context_left->note_pointer_position({});
            if (auto frame_to_present = context_left->clear_scrollbar_hover(); frame_to_present.has_value())
                request_present(*pointer.context_under_pointer, *frame_to_present);
        }
        pointer.context_under_pointer = nested_context_id;
    };

    switch (event.type) {
    case Type::MouseDown:
    case Type::MouseMove: {
        if (context_with_captured_scrollbar) {
            if (event.type == Type::MouseMove)
                return send_to_context_with_captured_scrollbar(event);
            // A press while a drag is still held means the release of that drag never came, so the drag ends there.
            auto cancel_event = event.clone_without_browser_data();
            cancel_event.type = Type::MouseCancel;
            (void)send_to_context_with_captured_scrollbar(cancel_event);
        }

        auto result = context.handle_mouse_event(event, now);
        if (!result.nested_context_mouse_hit.has_value()) {
            note_context_under_pointer({});
            return result;
        }

        auto frame_of_this_context = result.frame_to_present;
        auto containing_context_id = context_id;
        auto* containing_context = &context;
        Gfx::FloatPoint position;
        while (result.nested_context_mouse_hit.has_value()) {
            auto hit = *result.nested_context_mouse_hit;
            // WebContent names the contexts its display lists draw, so a context only reaches those nested in it.
            auto* nested_context = context_if_present(hit.child_context_id);
            Optional<Gfx::FloatPoint> position_in_nested_context_content;
            if (nested_context && nested_context->parent_context_id() == containing_context_id)
                position_in_nested_context_content = position_in_nested_context(hit, *nested_context);
            if (!position_in_nested_context_content.has_value()) {
                note_context_under_pointer({});
                auto declined = ContextState::ContextUpdateResult::declined(Web::InputRoutingDecision::NestedContextUnavailable);
                declined.frame_to_present = frame_of_this_context;
                declined.nested_context_hand_off = Web::InputRoutingDecision::HandedToNestedContext;
                return declined;
            }
            if (containing_context != &context) {
                apply_context_update_result(containing_context_id, *containing_context, result);
                containing_context->request_display_ticks_if_needed(now);
            }
            containing_context_id = hit.child_context_id;
            containing_context = nested_context;
            position = *position_in_nested_context_content;
            result = nested_context->handle_mouse_event_from_containing_context(event, position, now);
        }

        note_context_under_pointer(containing_context_id);
        if (event.type == Type::MouseDown && containing_context->has_captured_scrollbar()) {
            pointer.context_with_captured_scrollbar = containing_context_id;
            pointer.last_position_in_context_with_captured_scrollbar = position;
        }
        return result_of_nested_context(containing_context_id, *containing_context, result, move(frame_of_this_context));
    }
    case Type::MouseUp:
        if (context_with_captured_scrollbar)
            return send_to_context_with_captured_scrollbar(event);
        return context.handle_mouse_event(event, now);
    case Type::MouseLeave:
    case Type::MouseCancel: {
        // The pointer leaves every nested context it was in, and a cancel ends the drag of one, as in this context.
        Optional<ContextState::ContextUpdateResult> result_of_context_with_captured_scrollbar;
        Optional<Web::CompositorContextId> context_with_captured_scrollbar_id;
        if (context_with_captured_scrollbar) {
            context_with_captured_scrollbar_id = pointer.context_with_captured_scrollbar;
            result_of_context_with_captured_scrollbar = send_to_context_with_captured_scrollbar(event);
        }
        if (auto* context_under_pointer = context_still_nested_in(context_id, pointer.context_under_pointer); context_under_pointer && pointer.context_under_pointer != context_with_captured_scrollbar_id) {
            auto nested_result = context_under_pointer->handle_mouse_event_from_containing_context(event, {}, now);
            apply_context_update_result(*pointer.context_under_pointer, *context_under_pointer, nested_result);
            context_under_pointer->request_display_ticks_if_needed(now);
        }
        pointer.context_under_pointer.clear();

        auto result = context.handle_mouse_event(event, now);
        if (!result_of_context_with_captured_scrollbar.has_value())
            return result;
        result_of_context_with_captured_scrollbar->frame_to_present = result.frame_to_present;
        return result_of_context_with_captured_scrollbar.release_value();
    }
    case Type::MouseWheel:
        return context.handle_mouse_event(event, now);
    }
    VERIFY_NOT_REACHED();
}

// Whether a context nested in the context `context_id` scrolled since the last frame its WebContent presented: the lane
// of the context's document hands a pointer move to the lane of a same-process iframe, which hit tests it in the iframe.
bool CompositorCore::nested_context_scrolled_since_last_frame(Web::CompositorContextId context_id) const
{
    for (auto const& entry : m_contexts) {
        if (entry.value->scrolled_since_last_frame() && is_nested_in(context_id, *entry.value))
            return true;
    }
    return false;
}

bool CompositorCore::is_nested_in(Web::CompositorContextId containing_context_id, ContextState const& context) const
{
    // A walk up the parents that outlasts the number of contexts went around a loop.
    auto const* ancestor = &context;
    for (size_t remaining_ancestors = m_contexts.size(); ancestor && remaining_ancestors > 0; --remaining_ancestors) {
        auto parent_context_id = ancestor->parent_context_id();
        if (!parent_context_id.has_value())
            return false;
        if (*parent_context_id == containing_context_id)
            return true;
        ancestor = context_if_present(*parent_context_id);
    }
    return false;
}

ContextState::ContextUpdateResult CompositorCore::scroll_for_wheel_step(Web::CompositorContextId context_id, ContextState& context, Web::MouseEvent const& event, WheelStepOrigin origin, bool is_first_wheel_event_with_delta, Compositing::AsyncScrollOperationTracking operation_tracking, MonotonicTime now)
{
    auto gesture_id = context.wheel_gesture_id();
    auto listener_context_id = context.wheel_event_listener_context().value_or(context_id);
    auto* listener_context = listener_context_id == context_id ? &context : context_still_nested_in(context_id, listener_context_id);
    auto listener_gate = listener_context
        ? listener_context->wheel_event_listener_gate(gesture_id)
        : ContextState::WheelEventListenerGate { .reason_to_hold_latched_steps_back = Web::InputRoutingDecision::NestedContextUnavailable };

    ContextState::ContextUpdateResult result;
    bool presents_frame = false;
    if (listener_context && listener_context != &context && listener_gate.page_let_the_gesture_through && !context.has_wheel_latch_for_gesture(gesture_id)) {
        // The page of a nested context let the gesture through while nothing took its steps, so they go to that context
        // again rather than to the page.
        auto step = ContextState::WheelStep::for_wheel_event(event, gesture_id, operation_tracking);
        auto position = position_in_nested_context_ignoring_clips(context_id, context, *listener_context, step.position);
        if (!position.has_value())
            return ContextState::ContextUpdateResult::declined(Web::InputRoutingDecision::NestedContextUnavailable);
        step.position = *position;
        auto applied_step = route_wheel_step_the_page_let_through(context_id, context, listener_context_id, *listener_context, step, {}, {}, now);
        result = move(applied_step.result);
        result.nested_context_hand_off = Web::InputRoutingDecision::HandedToNestedContext;
        presents_frame = applied_step.presents_frame;
    } else {
        switch (origin) {
        case WheelStepOrigin::UI:
            result = context.handle_wheel_event(event, is_first_wheel_event_with_delta, listener_gate, now);
            break;
        case WheelStepOrigin::Fling:
            result = context.scroll_for_fling_step(event, listener_gate, now);
            break;
        case WheelStepOrigin::MainThread:
            result = context.scroll_for_wheel_event_from_main_thread(event, is_first_wheel_event_with_delta, listener_gate, operation_tracking, now);
            break;
        }
        presents_frame = result.frame_to_present.has_value();
        if (result.nested_context_wheel_step.has_value()) {
            auto applied_step = hand_wheel_step_to_nested_context(context_id, context, event, *result.nested_context_wheel_step, listener_gate, operation_tracking, now);
            result = move(applied_step.result);
            presents_frame = applied_step.presents_frame;
        }
    }

    // If the compositor accepts a momentum step of the fling but does not scroll, the scroller is at its edge.
    // Alternatively, a snap scroll uses the step. The next display tick ends the fling.
    if (origin == WheelStepOrigin::Fling && event.scroll_gesture_phase == Web::ScrollGesturePhase::Momentum && result.accepted && !presents_frame)
        context.end_scroll_fling_on_next_step();
    return result;
}

CompositorCore::AppliedWheelStep CompositorCore::apply_wheel_step_result(Web::CompositorContextId context_id, ContextState& context, ContextState::ContextUpdateResult const& result, WebContentClientId reporting_client, MonotonicTime now)
{
    apply_context_update_result(context_id, context, result);
    context.request_display_ticks_if_needed(now);
    // Node ids are only unique within the process that made them, and only that process follows the operations of the
    // context.
    bool ids_are_of_reporting_process = context.is_owned_by(reporting_client);
    return {
        .result = {
            .accepted = result.accepted,
            .routing_decision = result.routing_decision,
            .routed_scroller = ids_are_of_reporting_process ? result.routed_scroller : Optional<Web::AsyncScrollNodeStableID> {},
            .tracked_scroll_operation = ids_are_of_reporting_process ? result.tracked_scroll_operation : Optional<ContextState::TrackedScrollOperation> {},
        },
        .presents_frame = result.frame_to_present.has_value(),
    };
}

CompositorCore::AppliedWheelStep CompositorCore::hand_wheel_step_to_nested_context(Web::CompositorContextId context_id, ContextState& context, Web::MouseEvent const& event, ContextState::NestedContextWheelStep const& nested_step, ContextState::WheelEventListenerGate const& listener_gate, Compositing::AsyncScrollOperationTracking operation_tracking, MonotonicTime now)
{
    auto step = ContextState::WheelStep::for_wheel_event(event, context.wheel_gesture_id(), operation_tracking);

    if (!nested_step.hit.has_value()) {
        auto* nested_context = context_if_present(nested_step.context_id);
        if (!nested_context || !is_nested_in(context_id, *nested_context)) {
            context.forget_wheel_gesture_latch();
            return { ContextState::ContextUpdateResult::declined(Web::InputRoutingDecision::NestedContextUnavailable) };
        }
        // A blocking listener added during the gesture holds its later steps back until the page lets one through.
        if (listener_gate.reason_to_hold_latched_steps_back.has_value())
            return { ContextState::ContextUpdateResult::declined(*listener_gate.reason_to_hold_latched_steps_back) };
        auto admission = listener_gate.page_let_the_gesture_through ? ContextState::WheelStepAdmission::LetThroughByThePage : ContextState::WheelStepAdmission::ByThisPage;
        auto nested_result = nested_context->take_latched_wheel_step_from_containing_context(step, admission, now);
        if (nested_result.routing_decision == Web::InputRoutingDecision::NestedContextUnavailable)
            context.forget_wheel_gesture_latch();
        return apply_wheel_step_result(nested_step.context_id, *nested_context, nested_result, context.web_content_client_id(), now);
    }

    auto first_step = first_wheel_step_in_nested_contexts(context_id, *nested_step.hit, step.delta_in_css_pixels, step.wheel_delta_precision);
    context.set_wheel_event_listener_context(first_step.innermost_context_id);
    auto& taking_context = first_step.taking_context;
    if (!taking_context.has_value()) {
        auto result = context.scroll_for_wheel_event_passed_on_by_nested_context(event, operation_tracking, now);
        result.nested_context_hand_off = Web::InputRoutingDecision::NestedContextPassedStepOn;
        return { .result = result, .presents_frame = result.frame_to_present.has_value() };
    }

    AppliedWheelStep applied_step;
    if (taking_context->context && taking_context->route.kind != ContextState::FirstWheelStepRoute::Kind::Declined) {
        step.position = taking_context->position;
        auto nested_result = taking_context->context->take_first_wheel_step_from_containing_context(taking_context->route, step, now);
        applied_step = apply_wheel_step_result(taking_context->context_id, *taking_context->context, nested_result, context.web_content_client_id(), now);
        context.latch_wheel_gesture_to_nested_context(taking_context->context_id, applied_step.result.routed_scroller, step.scroll_gesture_phase);
    } else {
        applied_step.result = ContextState::ContextUpdateResult::declined(taking_context->route.decision);
    }
    applied_step.result.nested_context_hand_off = Web::InputRoutingDecision::HandedToNestedContext;
    return applied_step;
}

CompositorCore::FirstWheelStepInNestedContexts CompositorCore::first_wheel_step_in_nested_contexts(Web::CompositorContextId context_id, Compositing::NestedContextHit hit, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision wheel_delta_precision)
{
    using Kind = ContextState::FirstWheelStepRoute::Kind;
    // The contexts the step went into, outermost first, each nested in the one before.
    Vector<NestedContextTakingWheelStep, 4> contexts;
    auto containing_context_id = context_id;
    while (true) {
        auto declined_by_unavailable_context = [&] {
            return FirstWheelStepInNestedContexts {
                .innermost_context_id = hit.child_context_id,
                .taking_context = NestedContextTakingWheelStep { .context_id = hit.child_context_id, .route = { Kind::Declined, Web::InputRoutingDecision::NestedContextUnavailable } },
            };
        };
        // WebContent names the contexts its display lists draw, so a context only reaches those nested in it.
        auto* nested_context = context_if_present(hit.child_context_id);
        if (!nested_context || nested_context->parent_context_id() != containing_context_id)
            return declined_by_unavailable_context();
        auto position = position_in_nested_context(hit, *nested_context);
        if (!position.has_value())
            return declined_by_unavailable_context();
        auto route = nested_context->route_first_wheel_step_from_containing_context(*position, delta_in_css_pixels, wheel_delta_precision, Compositing::NestedContextRegions::HandToNestedContext, ContextState::WheelStepAdmission::ByThisPage);
        contexts.append({ .context_id = hit.child_context_id, .context = nested_context, .position = *position, .route = route });
        if (route.kind != Kind::NestedContext)
            break;
        containing_context_id = hit.child_context_id;
        hit = *route.nested_context_hit;
    }

    // What a nested context passes on goes to whatever is painted under it in the context it is nested in, whose page
    // does not see the step.
    auto innermost_context_id = contexts.last().context_id;
    while (contexts.last().route.kind == Kind::PassedOn) {
        contexts.take_last();
        if (contexts.is_empty())
            return { .innermost_context_id = innermost_context_id };
        auto& containing_context = contexts.last();
        containing_context.route = containing_context.context->route_first_wheel_step_from_containing_context(containing_context.position, delta_in_css_pixels, wheel_delta_precision, Compositing::NestedContextRegions::Ignore, ContextState::WheelStepAdmission::PassedOnByNestedContext);
    }
    return { .innermost_context_id = innermost_context_id, .taking_context = contexts.take_last() };
}

// The page of one process scrolls a context of another process only by what the UI left to the page in the gesture, so
// that a page cannot scroll the document around it at will.
static bool page_may_scroll_in(ContextState const& context, ContextState& routing_context, Optional<WebContentClientId> page_client, ContextState::WheelStep const& step)
{
    return !page_client.has_value() || context.is_owned_by(*page_client) || routing_context.take_wheel_delta_left_to_the_page(step.delta_in_css_pixels);
}

CompositorCore::AppliedWheelStep CompositorCore::route_wheel_step_the_page_let_through(Web::CompositorContextId routing_context_id, ContextState& routing_context, Web::CompositorContextId listener_context_id, ContextState& listener_context, ContextState::WheelStep step, ReadonlySpan<Web::AsyncScrollNodeStableID> scroll_chain, Optional<WebContentClientId> page_client, MonotonicTime now)
{
    using Kind = ContextState::FirstWheelStepRoute::Kind;
    auto const& reporting_client = page_client.has_value() ? *page_client : routing_context.web_content_client_id();
    auto declined = [](Web::InputRoutingDecision decision) {
        return AppliedWheelStep { .result = ContextState::ContextUpdateResult::declined(decision) };
    };

    auto context_id = listener_context_id;
    auto* context = &listener_context;
    // A walk up the parents that outlasts the number of contexts went around a loop.
    for (size_t remaining_contexts = m_contexts.size(); remaining_contexts > 0; --remaining_contexts) {
        if (context == &routing_context) {
            if (!page_may_scroll_in(routing_context, routing_context, page_client, step))
                return declined(Web::InputRoutingDecision::StepExceedsInputLeftToThePage);
            auto result = routing_context.scroll_for_first_wheel_step_the_page_let_through(step, scroll_chain, now);
            return apply_wheel_step_result(routing_context_id, routing_context, result, reporting_client, now);
        }

        auto route = context->route_first_wheel_step_from_containing_context(step.position, step.delta_in_css_pixels, step.wheel_delta_precision, Compositing::NestedContextRegions::Ignore, ContextState::WheelStepAdmission::LetThroughByThePage, scroll_chain);
        if (first_is_one_of(route.kind, Kind::ScrollNode, Kind::Kept)) {
            if (!page_may_scroll_in(*context, routing_context, page_client, step))
                return declined(Web::InputRoutingDecision::StepExceedsInputLeftToThePage);
            auto nested_result = context->take_first_wheel_step_from_containing_context(route, step, now);
            auto latched_scroller = context->is_owned_by(routing_context.web_content_client_id()) ? nested_result.routed_scroller : Optional<Web::AsyncScrollNodeStableID> {};
            routing_context.latch_wheel_gesture_to_nested_context(context_id, latched_scroller, step.scroll_gesture_phase);
            return apply_wheel_step_result(context_id, *context, nested_result, reporting_client, now);
        }
        if (route.kind != Kind::PassedOn)
            return declined(route.decision);

        // What a nested context passes on goes to whatever is painted under it in the context it is nested in.
        auto parent_context_id = context->parent_context_id();
        auto* parent_context = parent_context_id.has_value() ? context_if_present(*parent_context_id) : nullptr;
        if (!parent_context)
            return declined(Web::InputRoutingDecision::NestedContextUnavailable);
        auto position = parent_context->position_of_nested_context_position(context_id, step.position, context->viewport_size());
        if (!position.has_value())
            return declined(Web::InputRoutingDecision::NestedContextUnavailable);
        step.position = *position;
        scroll_chain = {};
        context_id = *parent_context_id;
        context = parent_context;
    }
    return declined(Web::InputRoutingDecision::NestedContextUnavailable);
}

Optional<Web::CompositorContextId> CompositorCore::routing_context_of_wheel_gesture(Web::CompositorContextId context_id, u64 gesture_id)
{
    // A walk up the parents that outlasts the number of contexts went around a loop.
    Optional<Web::CompositorContextId> candidate_context_id = context_id;
    for (size_t remaining_contexts = m_contexts.size(); candidate_context_id.has_value() && remaining_contexts > 0; --remaining_contexts) {
        auto* candidate_context = context_if_present(*candidate_context_id);
        if (!candidate_context)
            return {};
        if (candidate_context->wheel_gesture_id() == gesture_id)
            return candidate_context_id;
        candidate_context_id = candidate_context->parent_context_id();
    }
    return {};
}

void CompositorCore::dispatch_scroll_fling_step(Web::CompositorContextId context_id, ContextState& context, Optional<Web::MouseEvent> event, MonotonicTime now)
{
    // Send each fling step on the same route as a wheel event from the UI.
    if (!event.has_value())
        return;
    auto result = scroll_for_wheel_step(context_id, context, *event, WheelStepOrigin::Fling, false, Compositing::AsyncScrollOperationTracking::No, now);
    if (result.nested_context_hand_off.has_value())
        context.input_trace().record({ .input_event_id = event->id, .stage = Web::InputTraceStage::Compositor, .decision = *result.nested_context_hand_off });
    if (result.routing_decision.has_value())
        context.input_trace().record({ .input_event_id = event->id, .stage = Web::InputTraceStage::Compositor, .decision = *result.routing_decision, .scroller = result.routed_scroller });
    event->async_scroll_performed_default_action = apply_context_update_result(context_id, context, result);
    if (!event->async_scroll_performed_default_action)
        context.note_wheel_event_left_to_the_page(*event);
    context.request_display_ticks_if_needed(now);
    context.dispatch_mouse_event_to_web_content(*event);
}

// A pinch is a ctrl+wheel event first, to the document under it, whose blocking wheel event listeners may cancel it
// before anything zooms. Only the documents at the position can see that event.
bool CompositorCore::page_under_pinch_may_cancel_it(Web::CompositorContextId context_id, ContextState& context, Gfx::FloatPoint position)
{
    auto containing_context_id = context_id;
    auto* containing_context = &context;
    // A walk down the nested contexts that outlasts the number of contexts went around a loop.
    for (size_t remaining_contexts = m_contexts.size(); remaining_contexts > 0; --remaining_contexts) {
        if (containing_context->page_may_cancel_pinch_at(position))
            return true;
        auto hit = containing_context->nested_context_for_wheel_at(position);
        if (!hit.has_value())
            return false;
        // NB: Nothing is known yet of the listeners of a context that is not available.
        auto* nested_context = context_if_present(hit->child_context_id);
        if (!nested_context || nested_context->parent_context_id() != containing_context_id)
            return true;
        auto position_in_nested = position_in_nested_context(*hit, *nested_context);
        if (!position_in_nested.has_value())
            return true;
        containing_context_id = hit->child_context_id;
        containing_context = nested_context;
        position = *position_in_nested;
    }
    return true;
}

void CompositorCore::handle_and_dispatch_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent event)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    // NB: The presentation connection only passes on input for the contexts bound to it.
    auto* context = context_if_present(context_id);
    VERIFY(context);
    VERIFY(context->bound_presentation().has_value());
    auto presentation_id = *context->bound_presentation();
    if (!context->can_dispatch_input_to_web_content()) {
        post_to_presentation<Messages::CompositorPresentationClient::DidNotDispatchInputEvent>(presentation_id, context_id, event.id);
        return;
    }

    // The first step of a gesture decides for the steps after it: a gesture the page let through zooms from here on,
    // whatever listeners have appeared since, and any other leaves each step to the page, wherever the pinch has moved.
    // A page that already let this step through leaves only the zoom to do.
    Gfx::FloatPoint position { static_cast<float>(event.position.x().value()), static_cast<float>(event.position.y().value()) };
    auto may_zoom = event.continues_gesture ? context->pinch_gesture_was_let_through() : !page_under_pinch_may_cancel_it(context_id, *context, position);
    if (event.wheel_event_was_dispatched || may_zoom)
        event.async_zoom_performed_default_action = apply_context_update_result(context_id, *context, context->handle_pinch_event(event));
    context->set_pinch_gesture_was_let_through(event.async_zoom_performed_default_action);

    // The scroll updates the event produced reach WebContent ahead of the event itself on the same connection.
    publish_pending_async_scroll_updates(context_id, *context);
    context->dispatch_pinch_event_to_web_content(event);
    post_to_presentation<Messages::CompositorPresentationClient::DidDispatchInputEvent>(presentation_id, context_id, event.id, event.async_zoom_performed_default_action);
}

Compositing::WheelEventAnswer CompositorCore::wheel_event_answer_for(AppliedWheelStep const& applied_step, Optional<Web::WheelGestureMark> wheel_gesture_mark)
{
    auto const& tracked_scroll_operation = applied_step.result.tracked_scroll_operation;
    return {
        .performed_default_action = applied_step.result.accepted,
        .wheel_gesture_mark = wheel_gesture_mark,
        .operation_id = tracked_scroll_operation.map([](auto const& operation) { return operation.operation_id; }),
        .operation_context_id = tracked_scroll_operation.map([](auto const& operation) { return operation.context_id; }),
        .routing_decision = applied_step.result.routing_decision,
        .routed_scroller = applied_step.result.routed_scroller,
    };
}

Compositing::WheelEventAnswer CompositorCore::take_wheel_event_from_main_thread(Web::CompositorContextId context_id, Web::MouseEvent const& event, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelEventRouting routing, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto noted = context->note_wheel_event_from_main_thread(event, now);
    if (routing == Compositing::WheelEventRouting::AfterDispatch)
        return { .wheel_gesture_mark = noted.wheel_gesture_mark };
    auto result = scroll_for_wheel_step(context_id, *context, event, WheelStepOrigin::MainThread, noted.is_first_wheel_event_with_delta, operation_tracking, now);
    return wheel_event_answer_for(apply_wheel_step_result(context_id, *context, result, context->web_content_client_id(), now), noted.wheel_gesture_mark);
}

Compositing::WheelEventAnswer CompositorCore::scroll_for_wheel_event_the_page_let_through(Web::CompositorContextId listener_context_id, Compositing::WheelStepLetThrough const& let_through, Compositing::AsyncScrollOperationTracking operation_tracking, WebContentClientId page_client, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* listener_context = context_if_present(listener_context_id);
    VERIFY(listener_context);
    if (listener_context->document_id() != let_through.document_id)
        return { .routing_decision = Web::InputRoutingDecision::ScrollerInOtherDocument };

    auto gesture_id = let_through.wheel_gesture_mark.gesture_id;
    // A step that moves along no axis goes to the page as no wheel event, so the page had no say in letting it through,
    // and it decides nothing for the steps after it.
    if (!let_through.delta_in_css_pixels.is_zero())
        listener_context->note_page_let_wheel_gesture_through(gesture_id);
    ContextState::WheelStep step {
        .position = let_through.position,
        .delta_in_css_pixels = let_through.delta_in_css_pixels,
        .wheel_delta_precision = let_through.wheel_delta_precision,
        .scroll_gesture_phase = let_through.scroll_gesture_phase,
        .gesture_id = gesture_id,
        .operation_tracking = operation_tracking,
    };
    auto scroll_chain = let_through.scroll_chain.span();
    auto answer = [&](AppliedWheelStep const& applied_step) { return wheel_event_answer_for(applied_step, let_through.wheel_gesture_mark); };
    auto declined = [&](Web::InputRoutingDecision decision) { return answer({ .result = ContextState::ContextUpdateResult::declined(decision) }); };

    // A step of a gesture that the contexts around the page have moved past still scrolls, as the page let it.
    auto routing_context_id = routing_context_of_wheel_gesture(listener_context_id, gesture_id);
    auto* routing_context = routing_context_id.has_value() ? context_if_present(*routing_context_id) : nullptr;
    if (!routing_context || routing_context->wheel_step_was_overtaken(step.scroll_gesture_phase, let_through.wheel_gesture_mark.step_number)) {
        auto result = listener_context->scroll_for_overtaken_wheel_step(step, scroll_chain, now);
        return answer(apply_wheel_step_result(listener_context_id, *listener_context, result, page_client, now));
    }

    routing_context->set_wheel_event_listener_context(listener_context_id);
    // The end of a gesture lets go of the scroller it stretched.
    if (step.scroll_gesture_phase == Web::ScrollGesturePhase::Ended)
        routing_context->end_wheel_gesture_stretch(now);

    // A latched scroller that the page no longer has around the target of the step is gone from the page, and the step
    // is routed afresh.
    bool latch_is_to_scroller_of_this_page = *routing_context_id == listener_context_id && !routing_context->nested_context_of_wheel_latch().has_value();
    if (latch_is_to_scroller_of_this_page && !scroll_chain.is_empty()) {
        if (auto latched_scroller = routing_context->wheel_latched_scroller(); latched_scroller.has_value() && !scroll_chain.contains_slow(*latched_scroller))
            routing_context->forget_wheel_latch_of_gesture(gesture_id);
    }

    if (routing_context->has_wheel_latch_for_gesture(gesture_id)) {
        if (auto nested_context_id = routing_context->nested_context_of_wheel_latch(); nested_context_id.has_value()) {
            auto* nested_context = context_still_nested_in(*routing_context_id, nested_context_id);
            if (!nested_context) {
                routing_context->forget_wheel_latch_of_gesture(gesture_id);
                return declined(Web::InputRoutingDecision::NestedContextUnavailable);
            }
            if (!page_may_scroll_in(*nested_context, *routing_context, page_client, step))
                return declined(Web::InputRoutingDecision::StepExceedsInputLeftToThePage);
            auto nested_result = nested_context->take_latched_wheel_step_from_containing_context(step, ContextState::WheelStepAdmission::LetThroughByThePage, now);
            return answer(apply_wheel_step_result(*nested_context_id, *nested_context, nested_result, page_client, now));
        }
        if (!page_may_scroll_in(*routing_context, *routing_context, page_client, step))
            return declined(Web::InputRoutingDecision::StepExceedsInputLeftToThePage);
        if (auto result = routing_context->scroll_for_latched_wheel_step_the_page_let_through(step, now); result.has_value())
            return answer(apply_wheel_step_result(*routing_context_id, *routing_context, *result, page_client, now));
    }

    return answer(route_wheel_step_the_page_let_through(*routing_context_id, *routing_context, listener_context_id, *listener_context, step, scroll_chain, page_client, now));
}

void CompositorCore::forget_wheel_gesture_the_page_canceled(Web::CompositorContextId listener_context_id, u64 gesture_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* listener_context = context_if_present(listener_context_id);
    if (!listener_context)
        return;
    listener_context->forget_page_let_wheel_gesture_through(gesture_id);
    // The listener scrolls by itself, so the next step of the gesture is routed afresh.
    if (auto routing_context_id = routing_context_of_wheel_gesture(listener_context_id, gesture_id); routing_context_id.has_value())
        context_if_present(*routing_context_id)->forget_wheel_latch_of_gesture(gesture_id);
}

static bool scroll_request_smoothly_scrolls_for_script(Compositing::ScrollRequest const& scroll_request)
{
    return scroll_request.smooth_scroll.has_value() && scroll_request.smooth_scroll->initiator == Compositing::SmoothScrollInitiator::Programmatic;
}

void CompositorCore::apply_scroll_requests(Web::CompositorContextId context_id, ReadonlySpan<Compositing::ScrollRequest> scroll_requests, MonotonicTime now)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return;

    // A script's smooth scroll of a box ends the momentum scrolling it, as a key's does. A script's instant scroll only
    // moves the box, and the momentum goes on from there: pages keep their content in place under a flick that way.
    for (auto const& scroll_request : scroll_requests) {
        if (scroll_request.takes_over_from_momentum || scroll_request_smoothly_scrolls_for_script(scroll_request))
            dispatch_scroll_fling_step(context_id, *context, context->end_momentum_scrolling(scroll_request.stable_node_id), now);
    }
    if (auto frame_to_present = context->apply_scroll_requests(scroll_requests, now); frame_to_present.has_value())
        request_present(context_id, frame_to_present.value());
    context->request_display_ticks_if_needed(now);
    publish_pending_async_scroll_updates(context_id, *context);
}

Compositing::PendingAsyncScrollUpdates CompositorCore::take_pending_async_scroll_updates(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    VERIFY(context);

    return context->take_pending_async_scroll_updates();
}

void CompositorCore::publish_pending_async_scroll_updates(Web::CompositorContextId, ContextState& context)
{
    context.publish_pending_async_scroll_updates();
}

void CompositorCore::request_present(Web::CompositorContextId context_id, PendingFrame const& frame)
{
    m_present_requests.append({ context_id, frame.viewport_rect, frame.forced_damage_rect });
}

DisplayTickResult CompositorCore::display_tick(Web::CompositorContextId context_id, MonotonicTime frame_time, MonotonicTime now, DisplayTickInputs const& inputs)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context_pointer = context_if_present(context_id);
    VERIFY(context_pointer);
    auto& context = *context_pointer;

    auto has_active_animation_on_display = (context.needs_animation_frames(now) || inputs.ui_overlay_fades_need_frame) && inputs.context_is_on_tick_display;
    auto presents_on_display = inputs.present_is_scheduled_on_tick_display || has_active_animation_on_display;
    if (presents_on_display) {
        if (inputs.context_is_on_tick_display)
            dispatch_scroll_fling_step(context_id, context, context.take_scroll_fling_step(frame_time), frame_time);
        if (auto animation_frame = context.advance_smooth_scroll_animations(frame_time); animation_frame.has_value())
            m_present_requests.append({ context_id, *animation_frame, {}, PresentScheduling::WithDisplayTickInProgress });
        if (auto rebound_frame = context.advance_elastic_overscroll_rebounds(frame_time); rebound_frame.has_value())
            m_present_requests.append({ context_id, *rebound_frame, {}, PresentScheduling::WithDisplayTickInProgress });
        if (auto damage_rect = context.advance_scrollbar_fades_and_return_damage_rect(frame_time); !damage_rect.is_empty())
            m_present_requests.append({ context_id, {}, damage_rect, PresentScheduling::WithDisplayTickInProgress });
    }

    // A render clock tick goes to the process's render clock thread, beside what its main thread is doing, with
    // where the compositor has scrolled to for this vsync, which that main thread may not have taken in yet.
    if (inputs.clock_tick_interval_in_milliseconds.has_value())
        context.post_clock_tick(frame_time, *inputs.clock_tick_interval_in_milliseconds);

    if (presents_on_display) {
        publish_pending_async_scroll_updates(context_id, context);
        if (context.visual_animations_need_frame(now)) {
            context.advance_visual_animations(frame_time);
            m_present_requests.append({ context_id, {}, {}, PresentScheduling::WithDisplayTickInProgress });
        }
    }
    return { .presents_on_display = presents_on_display, .needs_animation_frames = context.needs_animation_frames(now) };
}

ContextState* CompositorCore::context_if_present(Web::CompositorContextId context_id)
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

ContextState const* CompositorCore::context_if_present(Web::CompositorContextId context_id) const
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

String CompositorCore::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller) const
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto const* context = context_if_present(context_id);
    if (!context)
        return {};
    return context->scroll_state_for_testing(stretched_scroller);
}

Optional<Compositing::FirstWheelStepRouteForTesting> CompositorCore::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return {};
    if (!context->presents_to_client() || !context->may_hand_first_wheel_steps_to_nested_contexts())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::LeaveToMainThread);
    auto hit = context->nested_context_for_wheel_at(position);
    if (!hit.has_value())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::HandToNestedContext);
    auto taking_context = first_wheel_step_in_nested_contexts(context_id, *hit, delta_in_css_pixels, Web::WheelDeltaPrecision::Precise).taking_context;
    if (!taking_context.has_value())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::Ignore);
    // Node ids are only unique within the process that made them.
    bool ids_are_of_this_process = taking_context->context && taking_context->context->is_owned_by(context->web_content_client_id());
    Compositing::FirstWheelStepRouteForTesting route { .route = { taking_context->route.decision }, .document_id = {} };
    if (ids_are_of_this_process) {
        route.route.scroller = taking_context->route.scroller;
        route.document_id = taking_context->context->document_id();
    }
    return route;
}

void CompositorCore::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    if (auto* context = context_if_present(context_id))
        context->input_trace().begin();
}

void CompositorCore::clear_per_test_input_state_for_testing(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    if (auto* context = context_if_present(context_id))
        context->clear_per_test_input_state();
}

Vector<Web::InputTraceRecord> CompositorCore::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    VERIFY(!m_is_entered);
    TemporaryChange entered { m_is_entered, true };

    auto* context = context_if_present(context_id);
    if (!context)
        return {};
    return context->input_trace().take();
}

bool CompositorCore::apply_context_update_result(
    Web::CompositorContextId context_id,
    ContextState& context,
    ContextState::ContextUpdateResult const& result)
{
    if (result.frame_to_present.has_value())
        request_present(context_id, *result.frame_to_present);
    publish_pending_async_scroll_updates(context_id, context);
    if (result.should_request_rendering_update)
        context.request_rendering_update();
    return result.accepted;
}

}
