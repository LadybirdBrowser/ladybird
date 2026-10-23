/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <Compositor/CompositorState.h>
#include <Compositor/ContextState.h>
#include <Compositor/DisplayListPlayerSkia.h>
#include <Compositor/PausedDebuggerOverlay.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <core/SkCanvas.h>
#include <core/SkImage.h>
#include <math.h>

namespace Compositor {

// INTEROP: Holding a press beside the thumb pages again after these delays, as in Chromium and Firefox.
static constexpr auto scrollbar_paging_initial_delay = AK::Duration::from_milliseconds(250);
static constexpr auto scrollbar_paging_repeat_interval = AK::Duration::from_milliseconds(50);

// INTEROP: The modifier that makes a press beside the thumb page instead of jumping there or the other way around, as in
//          Chromium and Firefox.
#if defined(AK_OS_MACOS)
static constexpr auto scrollbar_track_press_inverting_modifier = Web::UIEvents::Mod_Alt;
#else
static constexpr auto scrollbar_track_press_inverting_modifier = Web::UIEvents::Mod_Shift;
#endif

static Web::AsyncScrollNodeStableID viewport_stable_id_from(Compositing::AsyncScrollNodeID node_id)
{
    return {
        .node_id = node_id.document_id,
        .kind = Web::AsyncScrollNodeKind::Viewport,
    };
}

static void clamp_visual_viewport_transform_to_viewport(Compositing::TransformWithOrigin& transform, Gfx::IntRect viewport_rect)
{
    auto scale = transform.matrix[0, 0];
    if (scale <= 1.0f) {
        transform.matrix[0, 3] = 0;
        transform.matrix[1, 3] = 0;
        return;
    }

    auto min_x = -static_cast<float>(viewport_rect.width()) * (scale - 1.0f);
    auto min_y = -static_cast<float>(viewport_rect.height()) * (scale - 1.0f);
    transform.matrix[0, 3] = clamp(transform.matrix[0, 3], min_x, 0.0f);
    transform.matrix[1, 3] = clamp(transform.matrix[1, 3], min_y, 0.0f);
}

ContextState::ContextState(Web::CompositorContextId context_id, Optional<u64> page_id, CompositorStateWebContentClient& web_content_client, OwnedCanvasSurfaces canvas_surfaces, Function<void(Gfx::IntRect)> schedule_caret_repaint, Function<void()> request_animation_frames)
    : m_web_content_client(web_content_client)
    , m_context_id(context_id)
    , m_page_id(page_id)
    , m_presenter(m_scene, canvas_surfaces, page_id.has_value())
    , m_caret_blink_timer([this] { return damage_rect_of_blinking_carets(); }, move(schedule_caret_repaint))
    , m_request_animation_frames(move(request_animation_frames))
{
}

ContextState::~ContextState()
{
    if (m_scrollbar_paging_timer) {
        m_scrollbar_paging_timer->on_timeout = {};
        m_scrollbar_paging_timer->stop();
    }
    if (m_scrollbar_fade_out_timer) {
        m_scrollbar_fade_out_timer->on_timeout = {};
        m_scrollbar_fade_out_timer->stop();
    }
}

bool ContextState::is_owned_by(CompositorStateWebContentClient const& web_content_client) const
{
    return &m_web_content_client == &web_content_client;
}

void ContextState::request_rendering_update()
{
    // The rendering update this asks for adopts the scroll updates it finds pushed by then. What is pending here goes
    // ahead of the request on the wire, so that an update run on the request never misses it.
    if (has_pending_async_scroll_updates())
        m_web_content_client.async_scroll_updates(m_context_id, take_pending_async_scroll_updates());
    m_web_content_client.request_rendering_update();
}

void ContextState::dispatch_mouse_event_to_web_content(Web::MouseEvent const& event, bool nested_context_scrolled_since_last_frame)
{
    VERIFY(m_page_id.has_value());
    m_web_content_client.dispatch_mouse_event_to_web_content(*m_page_id, event);

    // The render clock hears where the pointer went too, without waiting for WebContent's main thread, so that hover
    // can follow the pointer while that thread runs a task. The lanes hit test it in the frames on screen, which a
    // scroll since, here or in a nested context, has moved.
    if (event.type == Web::MouseEvent::Type::MouseMove)
        m_web_content_client.pointer_moved(m_context_id, event.position, to_underlying(event.buttons), scrolled_since_last_frame() || nested_context_scrolled_since_last_frame, event.id);
    else if (event.type == Web::MouseEvent::Type::MouseLeave)
        m_web_content_client.pointer_left(m_context_id, event.id);
}

bool ContextState::scrolled_since_last_frame() const
{
    return has_pending_async_scroll_updates() || !m_async_scroll_update_outbox.unreconciled_scroll_offsets().is_empty() || has_active_smooth_scroll_animations() || has_active_scroll_fling();
}

void ContextState::dispatch_key_event_to_web_content(Web::KeyEvent const& event)
{
    VERIFY(m_page_id.has_value());
    m_web_content_client.dispatch_key_event_to_web_content(*m_page_id, event);
}

void ContextState::stop_presenting_to_client()
{
    end_keyboard_scroll_gesture();
    m_elastic_overscroll_controller.forget();
    m_presenter.stop_presenting_to_client();
}

void ContextState::set_parent_context(Optional<Web::CompositorContextId> parent_context_id)
{
    m_parent_context_id = parent_context_id;
}

void ContextState::apply_display_list_resource_transaction(Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    m_scene.apply_resource_transaction(move(resource_transaction));
}

void ContextState::install_display_list_update(
    NonnullRefPtr<Compositing::DisplayList> display_list,
    Compositing::AccumulatedVisualContextTree visual_context_tree,
    Compositing::ScrollStateSnapshot&& scroll_state_snapshot)
{
    VERIFY(display_list->compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    m_scene.install(move(display_list), move(visual_context_tree));
    m_keyboard_scroll_controller.forget_target();
    m_scroll_state_snapshot = move(scroll_state_snapshot);
    m_scroll_state_snapshot.set_node_count(m_scene.visual_context_tree()->spatial_node_count());
    m_async_scroll_update_outbox.retire_scroll_offsets_adopted_by(m_scroll_state_snapshot.adopted_async_scroll_sequence());
    m_caret_blink_timer.did_install_display_list(*m_scene.display_list());

    auto async_scrolling_state = Compositing::async_scrolling_state_from_display_list(*m_scene.display_list());
    auto async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
    m_wheel_gesture_routing.did_install_scrolling_state(async_scrolling_state);
    if (m_scene.has_async_visual_viewport_transform() && !m_wheel_gesture_routing.admits_async_pinch())
        m_scene.drop_async_visual_viewport_transform();

    auto was_dragging_scrollbar = m_scrollbar_controller.has_captured_scrollbar();
    m_scrollbar_controller.set_scrollbars(async_scrolling_state.scrollbars);
    if (!m_scrollbar_controller.has_captured_scrollbar())
        stop_scrollbar_paging();
    note_user_scroll_gesture_end_if_drag_ended(was_dragging_scrollbar);
    m_async_scroll_tree.set_state(move(async_scrolling_state));
    m_elastic_overscroll_controller.did_install_scrolling_state(m_async_scroll_tree);
    m_scroll_snap_controller.did_install_scrolling_state(m_async_scroll_tree);
    if (auto viewport_scroll_offset = reapply_unreconciled_async_scroll_offsets(); viewport_scroll_offset.has_value())
        async_scrolling_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    rebuild_wheel_hit_test_targets();
    m_async_scrolling_viewport_rect = async_scrolling_viewport_rect;
    m_has_async_scrolling_state = true;
    if (auto const& metadata = m_scene.display_list()->async_scrolling_metadata(); metadata.has_value())
        adopt_keyboard_scroll_state(metadata->keyboard_scroll_state);
}

Gfx::IntRect ContextState::damage_rect_of_blinking_carets()
{
    if (!m_scene.display_list() || !m_scene.visual_context_tree().has_value())
        return {};

    auto const& visual_context_tree = m_scene.visual_context_tree_for_compositing();
    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    Gfx::IntRect damage_rect;
    m_scene.display_list()->for_each_caret([&](auto context, auto bounding_rect, auto const& caret) {
        if (!caret.should_blink)
            return;
        auto rect = visual_context_tree.transform_rect_to_viewport(context.spatial, bounding_rect.template to_type<float>(), presented_scroll_state_snapshot);
        if (!isfinite(rect.x()) || !isfinite(rect.y()) || !isfinite(rect.width()) || !isfinite(rect.height())) {
            damage_rect = { {}, m_presenter.viewport_size() };
            return;
        }
        rect.intersect(Gfx::IntRect { {}, m_presenter.viewport_size() }.to_type<float>());
        if (!rect.is_empty())
            damage_rect.unite(Gfx::enclosing_int_rect(rect).inflated(1, 1, 1, 1));
    });
    damage_rect.intersect({ {}, m_presenter.viewport_size() });
    return damage_rect;
}

void ContextState::update_visual_context_tree(Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    if (!m_scene.replace_visual_context_tree(move(visual_context_tree), move(resource_transaction)))
        return;
    // A constraints refresh changes sticky payloads without a new snapshot, and the snapshot that
    // pairs with this tree arrives in a separate message.
    Compositing::resolve_sticky_offsets(*m_scene.visual_context_tree(), m_scroll_state_snapshot);

    if (m_has_async_scrolling_state)
        rebuild_wheel_hit_test_targets();
}

void ContextState::update_scroll_state(Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    m_scene.forget_whether_animated_content_may_affect_viewport();
    m_scroll_state_snapshot = move(scroll_state_snapshot);
    m_scroll_state_snapshot.set_node_count(m_scene.visual_context_tree().has_value() ? m_scene.visual_context_tree()->spatial_node_count() : 0);
    m_async_scroll_update_outbox.retire_scroll_offsets_adopted_by(m_scroll_state_snapshot.adopted_async_scroll_sequence());
    if (!m_has_async_scrolling_state)
        return;

    auto reconciled_viewport_scroll_offset = reapply_unreconciled_async_scroll_offsets();
    rebuild_wheel_hit_test_targets();
    if (reconciled_viewport_scroll_offset.has_value()) {
        auto reconciled_viewport_rect = m_async_scrolling_viewport_rect;
        reconciled_viewport_rect.set_location(reconciled_viewport_scroll_offset->to_type<int>());
        m_async_scrolling_viewport_rect = reconciled_viewport_rect;
    }
    adopt_keyboard_scroll_state(move(keyboard_scroll_state));
}

void ContextState::set_video_sink(Compositing::VideoSinkResourceId frame_id, RefPtr<Media::DisplayingVideoSink> sink)
{
    m_scene.set_video_sink(frame_id, move(sink));
}

void ContextState::invalidate_wheel_event_listener_state(u64 generation)
{
    m_wheel_gesture_routing.invalidate_wheel_event_listener_state(generation);
}

void ContextState::invalidate_keyboard_scroll_state(u64 generation)
{
    m_keyboard_scroll_controller.invalidate_state(generation);
}

void ContextState::adopt_keyboard_scroll_state(Compositing::KeyboardScrollState state)
{
    if (m_keyboard_scroll_controller.adopt_state(move(state)) && !m_keyboard_scroll_controller.has_target())
        end_keyboard_scroll_gesture();
}

void ContextState::end_keyboard_scroll_gesture()
{
    if (!m_keyboard_scroll_controller.release_all_keys())
        return;
    m_async_scroll_update_outbox.note_user_scroll_gesture_ended();
    request_rendering_update();
}

ContextState::ContextUpdateResult ContextState::handle_key_event(Web::KeyEvent const& event)
{
    if (!Web::is_keyboard_scroll_key(event.key, Web::UIEvents::Mod_None))
        return {};

    // Release the key even if focus, modifiers, or routing eligibility changed while it was held. Other scroll
    // keys may still be held, so only end the gesture when the last one is released.
    if (event.type == Web::KeyEvent::Type::KeyUp) {
        auto release = m_keyboard_scroll_controller.release_key(event);
        if (release == KeyboardScrollController::KeyRelease::Ignored)
            return {};
        if (release == KeyboardScrollController::KeyRelease::ReleasedLastHeldKey)
            m_async_scroll_update_outbox.note_user_scroll_gesture_ended();
        return { .accepted = true, .frame_to_present = {}, .should_request_rendering_update = true };
    }

    if (!presents_to_client() || m_presenter.ui_overlays().paused_debugger.has_value()
        || m_visibility != Compositing::ContextVisibility::Visible
        || !Web::is_keyboard_scroll_key(event.key, event.modifiers)
        || !m_keyboard_scroll_controller.has_target()
        || !m_scene.visual_context_tree().has_value()
        || !m_keyboard_scroll_controller.state_matches_visual_context_tree_epoch(m_scene.current_visual_context_tree().structural_epoch()))
        return {};
    // Keyboard scrolling has not yet been adapted to a separately panned visual viewport.
    if (m_scene.visual_viewport_scale().value_or(1.0f) != 1.0f)
        return {};

    auto key_step = m_keyboard_scroll_controller.step_for_key(event, m_async_scroll_tree);
    if (!key_step.has_value())
        return {};
    auto delta = key_step->delta;
    auto scrolls_to_extent = key_step->scrolls_to_extent;
    auto now = MonotonicTime::now();
    auto scroll_state_at_step_starts = scroll_state_snapshot_at_keyboard_step_starts();
    auto target = m_async_scroll_tree.scroll_node_for_keyboard_scroll(key_step->keyboard_scroll_target, delta, scroll_state_at_step_starts);
    // If every destination is at its boundary, consume repeats while a scrolling box can still move toward it.
    // Falling back would cancel the running animation and lose the steps already accumulated in its destination.
    if (!target.has_value())
        target = m_async_scroll_tree.scroll_node_for_keyboard_scroll(key_step->keyboard_scroll_target, delta, m_scroll_state_snapshot);
    if (!target.has_value())
        return {};

    m_keyboard_scroll_controller.hold_key(event.key);

    auto stable_node_id = m_async_scroll_tree.scroll_node_for_id(*target)->stable_node_id;
    Optional<Gfx::FloatPoint> scroll_in_flight_destination;
    for (auto const& running_animation : m_smooth_scroll_animations) {
        if (running_animation.stable_node_id == stable_node_id && running_animation.is_user_scroll && running_animation.animation.kind() == Compositing::ScrollAnimationKind::SmoothScroll)
            scroll_in_flight_destination = running_animation.animation.destination_offset();
    }
    auto css_scroll_in_flight_destination = scroll_in_flight_destination.map([&](auto offset) { return m_async_scroll_tree.css_pixels_from_device_offset(offset); });
    if (auto decision = m_scroll_snap_controller.decide_key_step(m_async_scroll_tree, m_scroll_state_snapshot, *target, m_async_scroll_tree.css_pixels_from_device_offset(delta), key_step->snap_selection_intent, css_scroll_in_flight_destination, now); decision.has_value()) {
        schedule_end_of_scroll_step_gestures(now);
        if (auto* snap_scroll = decision->get_pointer<ScrollSnapController::SnapScrollStart>()) {
            snap_scroll->is_absolute_scroll = scrolls_to_extent;
            start_snap_scroll(*target, move(*snap_scroll), false, now);
            return {
                .accepted = true,
                .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
                .should_request_rendering_update = true,
            };
        }
        if (auto& updated_scroll = decision->get<ScrollSnapController::StepConsumed>().updated_scroll; updated_scroll.has_value()) {
            updated_scroll->is_absolute_scroll = scrolls_to_extent;
            m_async_scroll_update_outbox.record_started_user_scroll(updated_scroll.release_value());
        }
        return { .accepted = true, .frame_to_present = {}, .should_request_rendering_update = true };
    }

    auto current_offset = m_async_scroll_tree.scroll_offset_for_node(*target, m_scroll_state_snapshot);
    VERIFY(current_offset.has_value());
    // A plain step continues toward the animation's destination, including any snap on the other axis.
    auto step_start = scroll_in_flight_destination.value_or(*current_offset);
    auto destination = m_async_scroll_tree.clamped_scroll_offset_for_node(*target, { step_start.x() + delta.x(), step_start.y() + delta.y() });
    if (destination == step_start)
        return { .accepted = true, .frame_to_present = {}, .should_request_rendering_update = true };

    for (auto& animation : m_smooth_scroll_animations) {
        if (animation.stable_node_id != stable_node_id || !animation.is_user_scroll || animation.animation.kind() != Compositing::ScrollAnimationKind::SmoothScroll)
            continue;
        retarget_user_scroll(animation, destination, now);
        report_started_user_scroll(animation.operation_id, stable_node_id, step_start, destination, Compositing::ScrollAnimationKind::SmoothScroll, scrolls_to_extent);
        return {
            .accepted = true,
            .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
            .should_request_rendering_update = true,
        };
    }

    cancel_smooth_scroll_taken_over_by_user_input(*target);
    auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
    m_smooth_scroll_animations.append({
        .stable_node_id = stable_node_id,
        .operation_id = operation_id,
        .animation = Compositing::SmoothScrollAnimation { *current_offset, destination, m_async_scroll_tree.device_pixels_per_css_pixel() },
        .started_at = now,
        .is_user_scroll = true,
    });
    report_started_user_scroll(operation_id, stable_node_id, step_start, destination, Compositing::ScrollAnimationKind::SmoothScroll, scrolls_to_extent);
    return {
        .accepted = true,
        .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
        .should_request_rendering_update = true,
    };
}

Compositing::ScrollStateSnapshot ContextState::scroll_state_snapshot_at_keyboard_step_starts() const
{
    Compositing::ScrollStateSnapshot snapshot { m_scroll_state_snapshot };
    for (auto const& running_animation : m_smooth_scroll_animations) {
        // A key continues a keyboard animation, but takes over other animations from the presented offset.
        if (!running_animation.is_user_scroll || running_animation.animation.kind() != Compositing::ScrollAnimationKind::SmoothScroll)
            continue;
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(running_animation.stable_node_id);
        if (!node_id.has_value())
            continue;
        auto destination = running_animation.animation.destination_offset();
        // A snap destination at the boundary can still consume steps until their accumulated input reaches it.
        if (auto unsnapped_destination = m_scroll_snap_controller.unsnapped_destination_for_snap_scroll(running_animation.stable_node_id, running_animation.operation_id); unsnapped_destination.has_value())
            destination = m_async_scroll_tree.device_offset_from_css_pixels(*unsnapped_destination);
        snapshot.set_device_offset_for_index(node_id->scroll_node_index, { -destination.x(), -destination.y() });
    }
    return snapshot;
}

ContextState::ContextUpdateResult ContextState::handle_mouse_event(Web::MouseEvent const& event)
{
    if (!presents_to_client())
        return ContextUpdateResult::declined(Web::InputRoutingDecision::ContextNotPresenting);

    auto position = Gfx::FloatPoint {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };

    if (m_scrollbar_controller.is_empty())
        return ContextUpdateResult::declined(Web::InputRoutingDecision::NotOverAScrollbar);

    auto scroller_of_scrollbar = [&](size_t scrollbar_index) {
        return m_scrollbar_controller.scrollbars()[scrollbar_index].scroller_stable_node_id;
    };

    // The compositor paints how its own scrollbars expand, outside of what damage tracking sees.
    auto frame_repainting_scrollbars_painted_by_compositor = [&]() -> Optional<PendingFrame> {
        if (m_async_scrolling_viewport_rect.is_empty())
            return {};
        return PendingFrame::repainting_everything(m_async_scrolling_viewport_rect);
    };

    switch (event.type) {
    case Web::MouseEvent::Type::MouseDown: {
        if (event.button != Web::UIEvents::MouseButton::Primary)
            return ContextUpdateResult::declined(Web::InputRoutingDecision::NotPrimaryButton);

        auto drag = m_scrollbar_controller.begin_drag(m_async_scroll_tree, m_scene.visual_context_tree_for_compositing(), m_scroll_state_snapshot, position);
        if (!drag.has_value())
            return ContextUpdateResult::declined(Web::InputRoutingDecision::NotOverAScrollbar);
        m_scrollbar_press_scroll_operation.clear();
        stop_scrollbar_paging();

        ContextUpdateResult result;
        result.accepted = true;
        result.routing_decision = Web::InputRoutingDecision::BeganScrollbarDrag;
        result.routed_scroller = scroller_of_scrollbar(drag->scrollbar_index);
        result.scrollbar_dragged_by_compositor = m_scrollbar_controller.captured_scrollbar_painted_by_display_list();
        if (!result.scrollbar_dragged_by_compositor.has_value())
            result.frame_to_present = frame_repainting_scrollbars_painted_by_compositor();

        // A press beside the thumb pages toward the pointer or jumps there, as the platform says, and a modifier swaps
        // the two.
        auto press_jumps = m_scrollbar_track_press_jumps != static_cast<bool>(event.modifiers & scrollbar_track_press_inverting_modifier);
        if (drag->is_press_beside_thumb && !press_jumps) {
            auto const& scrollbar = m_scrollbar_controller.scrollbars()[drag->scrollbar_index];
            auto scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, m_scroll_state_snapshot);
            VERIFY(scroll_offset.has_value());
            m_scrollbar_paging = ScrollbarPaging {
                .direction = m_scrollbar_controller.paging_direction(drag->scrollbar_index, drag->primary_position, *scroll_offset),
                .primary_position = drag->primary_position,
            };
            if (auto frame_to_present = page_captured_scrollbar(); frame_to_present.has_value()) {
                result.frame_to_present = *frame_to_present;
                result.should_request_rendering_update = true;
            }
            if (!m_scrollbar_paging_timer) {
                m_scrollbar_paging_timer = Core::Timer::create_single_shot(scrollbar_paging_initial_delay, [this] {
                    // Paging ends with the press, and with the scrollbar the press holds.
                    if (!m_scrollbar_paging.has_value() || !m_scrollbar_controller.has_captured_scrollbar()) {
                        stop_scrollbar_paging();
                        return;
                    }
                    // Once the thumb reaches the pointer, paging waits for the pointer to move.
                    if (!page_captured_scrollbar().has_value())
                        return;
                    request_rendering_update();
                    if (m_request_animation_frames)
                        m_request_animation_frames();
                    m_scrollbar_paging_timer->restart(scrollbar_paging_repeat_interval);
                });
            }
            m_scrollbar_paging_timer->restart(scrollbar_paging_initial_delay);
            return result;
        }

        if (auto frame_to_present = apply_scrollbar_drag(*drag); frame_to_present.has_value()) {
            result.frame_to_present = *frame_to_present;
            result.should_request_rendering_update = true;
        }
        return result;
    }
    case Web::MouseEvent::Type::MouseMove: {
        if (m_scrollbar_controller.has_captured_scrollbar()) {
            auto scrollbar_dragged_by_compositor = m_scrollbar_controller.captured_scrollbar_painted_by_display_list();
            auto drag = m_scrollbar_controller.captured_drag(m_scene.visual_context_tree_for_compositing(), m_scroll_state_snapshot, position);
            VERIFY(drag.has_value());
            // While paging, the pointer only says where the thumb pages to.
            Optional<PendingFrame> frame_to_present;
            if (m_scrollbar_paging.has_value()) {
                m_scrollbar_paging->primary_position = drag->primary_position;
                if (!m_scrollbar_paging_timer->is_active())
                    m_scrollbar_paging_timer->restart(scrollbar_paging_repeat_interval);
            } else {
                frame_to_present = apply_scrollbar_drag(*drag);
            }
            return {
                .accepted = true,
                .frame_to_present = frame_to_present,
                .should_request_rendering_update = frame_to_present.has_value(),
                .scrollbar_dragged_by_compositor = scrollbar_dragged_by_compositor,
                .routing_decision = Web::InputRoutingDecision::ContinuedScrollbarDrag,
                .routed_scroller = scroller_of_scrollbar(drag->scrollbar_index),
            };
        }

        // The main thread hovers the scrollbars the display list paints.
        auto hovered_scrollbar_index = m_scrollbar_controller.hit_test_scrollbar_painted_by_compositor(m_async_scroll_tree, m_scroll_state_snapshot, position);
        Optional<PendingFrame> frame_to_present;
        if (m_scrollbar_controller.set_hovered_scrollbar(hovered_scrollbar_index))
            frame_to_present = frame_repainting_scrollbars_painted_by_compositor();
        if (!hovered_scrollbar_index.has_value())
            return { .frame_to_present = frame_to_present, .routing_decision = Web::InputRoutingDecision::NotOverAScrollbar };
        return {
            .accepted = true,
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = false,
            .routing_decision = Web::InputRoutingDecision::HoveringScrollbarPaintedByCompositor,
            .routed_scroller = scroller_of_scrollbar(*hovered_scrollbar_index),
        };
    }
    case Web::MouseEvent::Type::MouseUp: {
        if (event.button != Web::UIEvents::MouseButton::Primary)
            return ContextUpdateResult::declined(Web::InputRoutingDecision::NotPrimaryButton);

        auto was_dragging_scrollbar = m_scrollbar_controller.has_captured_scrollbar();
        auto scrollbar_dragged_by_compositor = m_scrollbar_controller.captured_scrollbar_painted_by_display_list();
        auto drag = m_scrollbar_controller.release_captured_drag(m_scene.visual_context_tree_for_compositing(), m_scroll_state_snapshot, position);
        auto was_paging = m_scrollbar_paging.has_value();
        stop_scrollbar_paging();
        if (!drag.has_value())
            return ContextUpdateResult::declined(Web::InputRoutingDecision::NoScrollbarDrag);

        note_user_scroll_gesture_end_if_drag_ended(was_dragging_scrollbar);

        ContextUpdateResult result;
        result.accepted = true;
        result.routing_decision = Web::InputRoutingDecision::EndedScrollbarDrag;
        result.routed_scroller = scroller_of_scrollbar(drag->scrollbar_index);
        result.scrollbar_dragged_by_compositor = scrollbar_dragged_by_compositor;
        // The main thread learns of the release from the next update it takes, so one is asked for even when the
        // release scrolls nothing.
        result.should_request_rendering_update = true;
        if (!scrollbar_dragged_by_compositor.has_value())
            result.frame_to_present = frame_repainting_scrollbars_painted_by_compositor();
        if (!was_paging) {
            if (auto frame_to_present = apply_scrollbar_drag(*drag); frame_to_present.has_value())
                result.frame_to_present = *frame_to_present;
        }

        auto hovered_scrollbar_index = m_scrollbar_controller.hit_test_scrollbar_painted_by_compositor(m_async_scroll_tree, m_scroll_state_snapshot, position);
        if (m_scrollbar_controller.set_hovered_scrollbar(hovered_scrollbar_index)) {
            if (auto frame_to_present = frame_repainting_scrollbars_painted_by_compositor(); frame_to_present.has_value())
                result.frame_to_present = frame_to_present;
        }

        return result;
    }
    case Web::MouseEvent::Type::MouseLeave: {
        auto had_capture = m_scrollbar_controller.has_captured_scrollbar();
        Optional<PendingFrame> frame_to_present;
        if (m_scrollbar_controller.set_hovered_scrollbar({}))
            frame_to_present = frame_repainting_scrollbars_painted_by_compositor();
        return {
            .accepted = had_capture,
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = false,
            .scrollbar_dragged_by_compositor = m_scrollbar_controller.captured_scrollbar_painted_by_display_list(),
            .routing_decision = had_capture ? Web::InputRoutingDecision::LeftDuringScrollbarDrag : Web::InputRoutingDecision::NoScrollbarDrag,
        };
    }
    case Web::MouseEvent::Type::MouseCancel: {
        // A cancel ends a drag where it last scrolled to, as a cancel has no position of its own, and the pointer then
        // counts as having left. The page still sees the cancel, so it is not accepted.
        auto was_dragging_scrollbar = m_scrollbar_controller.has_captured_scrollbar();
        auto scrollbar_dragged_by_compositor = m_scrollbar_controller.captured_scrollbar_painted_by_display_list();
        (void)m_scrollbar_controller.release_captured_drag(m_scene.visual_context_tree_for_compositing(), m_scroll_state_snapshot, position);
        stop_scrollbar_paging();
        note_user_scroll_gesture_end_if_drag_ended(was_dragging_scrollbar);
        auto hovered_scrollbar_changed = m_scrollbar_controller.set_hovered_scrollbar({});

        ContextUpdateResult result;
        // As for a release, the main thread learns that the drag ended from the next update it takes.
        result.should_request_rendering_update = was_dragging_scrollbar;
        if ((was_dragging_scrollbar && !scrollbar_dragged_by_compositor.has_value()) || hovered_scrollbar_changed)
            result.frame_to_present = frame_repainting_scrollbars_painted_by_compositor();
        return result;
    }
    case Web::MouseEvent::Type::MouseWheel:
        return {};
    }

    VERIFY_NOT_REACHED();
}

ContextState::ContextUpdateResult ContextState::handle_pinch_event(Web::PinchEvent const& event)
{
    if (!presents_to_client())
        return {};
    if (!m_wheel_gesture_routing.admits_async_pinch())
        return {};
    if (!m_scene.visual_context_tree().has_value())
        return {};

    auto scale = 1.0 + event.scale_delta;
    if (scale == 1.0)
        return {};

    auto transform = m_scene.visual_viewport_transform();
    auto old_scale = transform.matrix[0, 0];
    if (old_scale <= 0)
        return {};

    auto new_scale = clamp(static_cast<double>(old_scale) * scale, 1.0, 5.0);
    auto applied_scale = static_cast<float>(new_scale / old_scale);
    if (applied_scale == 1.0f)
        return {};

    auto position = Gfx::FloatPoint {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };
    auto gesture_transform = Gfx::translation_matrix(Gfx::FloatVector3 { position.x(), position.y(), 0 })
        * Gfx::scale_matrix(Gfx::FloatVector3 { applied_scale, applied_scale, 1 })
        * Gfx::translation_matrix(Gfx::FloatVector3 { -position.x(), -position.y(), 0 });

    transform.matrix = gesture_transform * transform.matrix;
    clamp_visual_viewport_transform_to_viewport(transform, m_async_scrolling_viewport_rect);
    m_scene.set_async_visual_viewport_transform(transform);
    rebuild_wheel_hit_test_targets();

    return {
        .accepted = true,
        .frame_to_present = PendingFrame::repainting_everything(m_async_scrolling_viewport_rect),
        .should_request_rendering_update = false,
    };
}

Compositing::AsyncScrollOperationID ContextState::start_snap_scroll(Compositing::AsyncScrollNodeID node_id, ScrollSnapController::SnapScrollStart&& snap_scroll, bool settles_gesture, MonotonicTime now)
{
    auto const& stable_node_id = snap_scroll.stable_node_id;
    // A snap scroll of this context's own that is still running is replaced by the new one; any other scroll of the
    // box is taken over by the input.
    auto running_animation = m_smooth_scroll_animations.find_if([&](auto const& animation) { return animation.stable_node_id == stable_node_id; });
    if (running_animation != m_smooth_scroll_animations.end()) {
        if (!m_scroll_snap_controller.is_snap_scroll(stable_node_id, running_animation->operation_id))
            m_async_scroll_update_outbox.note_operation_taken_over_by_user_input(running_animation->operation_id);
        // Keep the accumulation state the controller just recorded when selecting this snap destination.
        retire_smooth_scroll_animation(stable_node_id);
    }

    auto current_offset = m_async_scroll_tree.scroll_offset_for_node(node_id, m_scroll_state_snapshot);
    VERIFY(current_offset.has_value());
    auto destination = snap_scroll.selection.position;
    auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
    m_smooth_scroll_animations.append({
        .stable_node_id = stable_node_id,
        .operation_id = operation_id,
        .animation = Compositing::SmoothScrollAnimation { *current_offset, m_async_scroll_tree.device_offset_from_css_pixels(destination), m_async_scroll_tree.device_pixels_per_css_pixel(), snap_scroll.animation_kind },
        .started_at = now,
        .is_user_scroll = true,
    });
    m_scroll_snap_controller.did_start_snap_scroll(stable_node_id, operation_id, destination);
    m_async_scroll_update_outbox.record_started_user_scroll({
        .stable_node_id = stable_node_id,
        .operation_id = operation_id,
        .initial_scroll_offset = snap_scroll.initial_scroll_offset,
        .unsnapped_scroll_destination = snap_scroll.unsnapped_scroll_destination,
        .relative_scroll_delta = snap_scroll.relative_scroll_delta,
        .selection = move(snap_scroll.selection),
        .settles_gesture = settles_gesture,
        .is_absolute_scroll = snap_scroll.is_absolute_scroll,
        .animation_kind = snap_scroll.animation_kind,
    });
    return operation_id;
}

// The end of a gesture snaps the boxes it panned; the first snap scroll it starts is the operation a caller follows.
Optional<Compositing::AsyncScrollOperationID> ContextState::snap_at_gesture_end(MonotonicTime now)
{
    Optional<Compositing::AsyncScrollOperationID> operation_id;
    for (auto& snap : m_scroll_snap_controller.decide_gesture_end(m_async_scroll_tree, m_scroll_state_snapshot)) {
        auto started_operation_id = start_snap_scroll(snap.node_id, move(snap.snap_scroll), true, now);
        if (!operation_id.has_value())
            operation_id = started_operation_id;
    }
    return operation_id;
}

// The viewport rect a wheel scroll presents, moved along with the viewport when the scroll moved it.
Gfx::IntRect ContextState::note_async_scrolling_viewport_rect(Gfx::IntRect viewport_rect, Optional<Compositing::AsyncScrollOffset> const& scroll_offset)
{
    if (scroll_offset.has_value()) {
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(scroll_offset->stable_node_id);
        if (node_id.has_value() && m_async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_rect.set_location(scroll_offset->compositor_scroll_offset.to_type<int>());
    }
    m_async_scrolling_viewport_rect = viewport_rect;
    return viewport_rect;
}

ContextState::WheelScrollOutcome ContextState::perform_wheel_scroll_of_node(Compositing::AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, Compositing::AsyncScrollOperationTracking operation_tracking, Gfx::IntRect viewport_rect, MonotonicTime now, Compositing::ScrollChaining scroll_chaining, VisualViewportPanning visual_viewport_panning)
{
    bool pans_visual_viewport = visual_viewport_panning == VisualViewportPanning::BeforeScrolling;
    auto magnification = pans_visual_viewport ? m_scene.visual_viewport_magnification() : 1.0f;
    auto step_delta = delta.scaled(1.0f / magnification);

    auto const* latched_node = m_async_scroll_tree.scroll_node_for_id(node_id);
    bool step_can_stretch_elastically = wheel_step_can_stretch_elastically(wheel_delta_precision, scroll_gesture_phase);
    bool fingers_can_stretch_latched_scroller = step_can_stretch_elastically && scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing && latched_node;
    bool momentum_can_bounce_latched_scroller = step_can_stretch_elastically && scroll_gesture_phase == Web::ScrollGesturePhase::Momentum && latched_node
        && m_wheel_gesture_routing.momentum_of_latched_gesture_can_spring_scroller_back();
    auto presented_stretch_of_latched_scroller = [&]() -> Gfx::FloatPoint {
        if (!latched_node)
            return {};
        return m_elastic_overscroll_controller.stretch_of(latched_node->stable_node_id);
    };
    auto stretch_before_step = presented_stretch_of_latched_scroller();

    // Momentum only arrives once the fingers have lifted, and its first step can be the first sign that they did.
    if (step_can_stretch_elastically && scroll_gesture_phase == Web::ScrollGesturePhase::Momentum)
        m_elastic_overscroll_controller.release_held_by_fingers(now);
    if (momentum_can_bounce_latched_scroller && m_elastic_overscroll_controller.has_active_overscroll_of(latched_node->stable_node_id)) {
        // The fingers lifted from a stretched scroller, which this momentum already springs back with.
        m_wheel_gesture_routing.set_momentum_of_latched_gesture_has_sprung_scroller_back(true);
        momentum_can_bounce_latched_scroller = false;
    }

    // A stretched scroller is past the edge of everything the gesture moves, so the fingers relax the stretch before
    // they pan the visual viewport away from its own edge.
    auto delta_left = delta;
    if (fingers_can_stretch_latched_scroller) {
        m_wheel_gesture_routing.set_momentum_of_latched_gesture_has_sprung_scroller_back(false);
        auto step_delta_left = m_elastic_overscroll_controller.relax_by(latched_node->stable_node_id, step_delta);
        if (step_delta_left != step_delta)
            delta_left = step_delta_left.scaled(magnification);
    }

    WheelScrollOutcome outcome;
    bool visual_viewport_took_rest_of_step = false;
    if (pans_visual_viewport) {
        auto panned_delta = pan_visual_viewport_by(delta_left);
        if (!panned_delta.is_zero()) {
            delta_left -= panned_delta;
            visual_viewport_took_rest_of_step = delta_left.is_zero();
            outcome.viewport_rect_to_present = note_async_scrolling_viewport_rect(viewport_rect, {});
        }
    }

    Optional<Gfx::FloatPoint> delta_the_scroller_did_not_scroll_by;
    if (!visual_viewport_took_rest_of_step) {
        auto node_scroll = scroll_node_by_wheel_delta(node_id, delta_left.scaled(1.0f / magnification), wheel_delta_precision, scroll_gesture_phase, operation_tracking, viewport_rect, now, scroll_chaining);
        outcome.operation_id = node_scroll.outcome.operation_id;
        if (node_scroll.outcome.viewport_rect_to_present.has_value())
            outcome.viewport_rect_to_present = node_scroll.outcome.viewport_rect_to_present;
        delta_the_scroller_did_not_scroll_by = node_scroll.delta_the_node_did_not_scroll_by;
    }

    if (delta_the_scroller_did_not_scroll_by.has_value()) {
        if (fingers_can_stretch_latched_scroller) {
            pull_elastic_overscroll_by(*latched_node, step_delta, *delta_the_scroller_did_not_scroll_by);
        } else if (momentum_can_bounce_latched_scroller && pull_elastic_overscroll_by(*latched_node, step_delta, *delta_the_scroller_did_not_scroll_by)) {
            // Momentum that carries a scroller into its edge bounces it: the velocity it arrives with takes the
            // rebound out past the edge before the rebound brings it back.
            m_elastic_overscroll_controller.release_held_by_fingers(now);
            m_wheel_gesture_routing.set_momentum_of_latched_gesture_has_sprung_scroller_back(true);
        }
    }

    if (!outcome.viewport_rect_to_present.has_value() && presented_stretch_of_latched_scroller() != stretch_before_step) {
        outcome.viewport_rect_to_present = note_async_scrolling_viewport_rect(viewport_rect, {});
        outcome.only_elastic_overscroll_changed = true;
    }
    return outcome;
}

ContextState::NodeScrollByWheelDelta ContextState::scroll_node_by_wheel_delta(Compositing::AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, Compositing::AsyncScrollOperationTracking operation_tracking, Gfx::IntRect viewport_rect, MonotonicTime now, Compositing::ScrollChaining scroll_chaining)
{
    WheelScrollOutcome outcome;
    Optional<Compositing::WheelScrollTarget> wheel_scroll_target;
    if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete) {
        wheel_scroll_target = m_async_scroll_tree.select_wheel_scroll_target(node_id, delta, m_scroll_state_snapshot, [this](auto stable_node_id) { return in_flight_wheel_destination(stable_node_id); }, scroll_chaining);
        if (wheel_scroll_target.has_value()) {
            node_id = *m_async_scroll_tree.scroll_node_id_for_stable_id(wheel_scroll_target->stable_node_id);
            m_wheel_gesture_routing.relatch_to(wheel_scroll_target->stable_node_id);
        }
    }
    auto css_delta = m_async_scroll_tree.css_pixels_from_device_offset(delta);

    Optional<ScrollSnapController::StepDecision> decision;
    if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete) {
        Optional<Web::CSSPixelPoint> scroll_in_flight_destination;
        if (auto const* node = m_async_scroll_tree.scroll_node_for_id(node_id))
            scroll_in_flight_destination = in_flight_wheel_destination(node->stable_node_id).map([&](auto offset) { return m_async_scroll_tree.css_pixels_from_device_offset(offset); });
        decision = m_scroll_snap_controller.decide_discrete_step(m_async_scroll_tree, m_scroll_state_snapshot, node_id, css_delta, scroll_in_flight_destination, now);
    } else if (scroll_gesture_phase == Web::ScrollGesturePhase::Momentum) {
        decision = m_scroll_snap_controller.decide_momentum_delta(m_async_scroll_tree, m_scroll_state_snapshot, node_id, css_delta);
    }

    if (decision.has_value()) {
        schedule_end_of_scroll_step_gestures(now);
        if (auto* snap_scroll = decision->get_pointer<ScrollSnapController::SnapScrollStart>()) {
            if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete)
                snap_scroll->animation_kind = Compositing::ScrollAnimationKind::Wheel;
            auto operation_id = start_snap_scroll(node_id, move(*snap_scroll), false, now);
            if (operation_tracking == Compositing::AsyncScrollOperationTracking::Yes)
                outcome.operation_id = operation_id;
            outcome.viewport_rect_to_present = note_async_scrolling_viewport_rect(viewport_rect, {});
            return { outcome, {} };
        }
        if (operation_tracking == Compositing::AsyncScrollOperationTracking::Yes) {
            outcome.operation_id = m_async_scroll_update_outbox.allocate_operation_id();
            m_async_scroll_update_outbox.complete_operation(*outcome.operation_id);
        }
        return { outcome, {} };
    }

    if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete) {
        auto result = smooth_wheel_scroll_by(wheel_scroll_target, viewport_rect, now);
        outcome.operation_id = result.enqueue_result.operation_id;
        if (result.frame_to_present.has_value())
            outcome.viewport_rect_to_present = viewport_rect;
        return { outcome, {} };
    }

    cancel_smooth_scroll_taken_over_by_user_input(node_id);
    if (operation_tracking == Compositing::AsyncScrollOperationTracking::Yes)
        outcome.operation_id = m_async_scroll_update_outbox.allocate_operation_id();

    auto scroll_offset = m_async_scroll_tree.apply_scroll_delta(node_id, delta, m_scene.current_visual_context_tree(), m_scroll_state_snapshot, scroll_chaining);
    if (!scroll_offset.has_value()) {
        if (outcome.operation_id.has_value())
            m_async_scroll_update_outbox.complete_operation(*outcome.operation_id);
        return { outcome, delta };
    }

    // On the first step of a gesture, scroll node chaining may have scrolled an ancestor instead, which took the delta.
    auto const* node = m_async_scroll_tree.scroll_node_for_id(node_id);
    bool an_ancestor_scrolled = node && scroll_offset->stable_node_id != node->stable_node_id;
    Optional<Gfx::FloatPoint> delta_the_node_did_not_scroll_by;
    if (!an_ancestor_scrolled)
        delta_the_node_did_not_scroll_by = delta - scroll_offset->unadopted_scroll_delta;

    // The box the delta moved is the one a gesture pans; on the first step of a gesture, scroll node chaining may have
    // moved an ancestor of the node the gesture latched to.
    auto scroll_offset_before_scroll = scroll_offset->compositor_scroll_offset - scroll_offset->unadopted_scroll_delta;
    m_scroll_snap_controller.did_scroll_node_plainly(scroll_offset->stable_node_id, scroll_gesture_phase, m_async_scroll_tree.css_pixels_from_device_offset(scroll_offset_before_scroll));

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // A wheel or panning scroll is relative, which scroll-state(scrolled) queries observe.
    scroll_offset->last_relative_scroll_delta = scroll_offset->unadopted_scroll_delta;

    rebuild_wheel_hit_test_targets();
    store_pending_async_scroll_offsets({ &*scroll_offset, 1 }, outcome.operation_id);
    outcome.viewport_rect_to_present = note_async_scrolling_viewport_rect(viewport_rect, scroll_offset);
    return { outcome, delta_the_node_did_not_scroll_by };
}

ContextState::AsyncScrollResult ContextState::async_scroll_by(
    Web::UniqueNodeID expected_document_id,
    Gfx::FloatPoint position,
    Gfx::FloatPoint delta,
    Gfx::IntRect viewport_rect,
    Web::WheelDeltaPrecision wheel_delta_precision,
    Web::ScrollGesturePhase scroll_gesture_phase,
    u32 modifiers,
    Compositing::AsyncScrollOperationTracking operation_tracking,
    Compositing::WheelStepDispatch wheel_step_dispatch,
    u64 input_event_id,
    Optional<MonotonicTime> now_for_testing)
{
    auto now = now_for_testing.value_or(MonotonicTime::now());

    // Nothing hit tests a wheel before the first display list shows where the scrollers are, and the page can let a
    // touchpad step through to here before then.
    if (!m_has_async_scrolling_state)
        return AsyncScrollResult::declined(Web::InputRoutingDecision::NoAsyncScrollingState);

    // The UI hands this context every wheel event before WebContent's main thread passes it on. A step that the main
    // thread passes on after the UI sent one it cannot come before must neither latch a scroller nor pull a stretch,
    // since nothing would be left to let go of either.
    bool step_came_from_ui = input_event_id != 0 && presents_to_client();
    if (step_came_from_ui && m_wheel_gesture_routing.step_was_overtaken_by_ui(scroll_gesture_phase, input_event_id))
        return AsyncScrollResult::declined(Web::InputRoutingDecision::StepOvertakenByUI);

    // The end of a gesture lets go of the scroller it stretched even if the context has stopped taking wheel events.
    if (scroll_gesture_phase == Web::ScrollGesturePhase::Ended)
        m_elastic_overscroll_controller.end_gesture(now);

    // Blocking wheel event listeners have nothing left to hold back once the page has dispatched a step and did not
    // cancel it. The compositor then scrolls the step of a touchpad gesture, so that the gesture stretches the edges
    // it pulls past as it does where nothing holds its steps back.
    bool page_let_touchpad_step_through = wheel_step_dispatch == Compositing::WheelStepDispatch::DispatchedAndNotCanceled
        && step_came_from_ui
        && wheel_delta_precision == Web::WheelDeltaPrecision::Precise
        && first_is_one_of(scroll_gesture_phase, Web::ScrollGesturePhase::Ongoing, Web::ScrollGesturePhase::Momentum);
    bool only_blocking_wheel_event_listeners_hold_steps_back = m_wheel_gesture_routing.only_blocking_wheel_event_listeners_hold_steps_back();

    auto latch = resolve_wheel_scroll_latch(position, scroll_gesture_phase, modifiers, now);
    // The compositor hands every step of a gesture latched to a nested context's scroller to that context itself.
    if (latch.nested_context_id.has_value())
        return AsyncScrollResult::declined(Web::InputRoutingDecision::ScrollerInOtherDocument);
    auto latched_node_id = latch.node_id;
    // The end of the gesture before this one may never have arrived. Whether a step of the UI starts another gesture
    // was decided when the UI sent it.
    if (!latched_node_id.has_value() && !step_came_from_ui)
        m_elastic_overscroll_controller.end_gesture(now);

    if (!m_wheel_gesture_routing.admits_async_wheel()
        && !step_continues_touchpad_gesture_the_page_let_through(latched_node_id, wheel_delta_precision, scroll_gesture_phase)
        && !(page_let_touchpad_step_through && only_blocking_wheel_event_listeners_hold_steps_back))
        return AsyncScrollResult::declined(Compositing::input_routing_decision_for_wheel_step_not_admitted_by(m_wheel_gesture_routing.admission()));

    m_scroll_snap_controller.note_gesture_phase(scroll_gesture_phase);

    // The end of a gesture carries no delta to hit test; it snaps the boxes the gesture panned.
    if (scroll_gesture_phase == Web::ScrollGesturePhase::Ended && delta.is_zero()) {
        auto operation_id = snap_at_gesture_end(now);
        if (!operation_id.has_value())
            return AsyncScrollResult::declined(Web::InputRoutingDecision::GestureEndWithNothingToSnap);
        m_async_scrolling_viewport_rect = viewport_rect;
        return {
            .enqueue_result = {
                .accepted = true,
                .operation_id = operation_tracking == Compositing::AsyncScrollOperationTracking::Yes ? operation_id : Optional<Compositing::AsyncScrollOperationID> {},
                .routing_decision = Web::InputRoutingDecision::SnappedAtGestureEnd,
            },
            .frame_to_present = PendingFrame::repainting_changes(viewport_rect),
        };
    }

    bool is_first_step_of_gesture = !latched_node_id.has_value();
    auto routing_decision = Web::InputRoutingDecision::ContinuedLatch;
    if (is_first_step_of_gesture) {
        auto wheel_step_dispatch_for_hit_test = page_let_touchpad_step_through ? Compositing::WheelStepDispatch::DispatchedAndNotCanceled : Compositing::WheelStepDispatch::NotYetDispatched;
        auto latch_attempt = hit_test_and_latch_wheel_gesture(position, delta, wheel_delta_precision, scroll_gesture_phase, modifiers, now, expected_document_id, wheel_step_dispatch_for_hit_test, Compositing::NestedContextRegions::LeaveToMainThread);
        if (!latch_attempt.latched_node_id.has_value())
            return AsyncScrollResult::declined(latch_attempt.decision);
        latched_node_id = latch_attempt.latched_node_id;
        routing_decision = latch_attempt.decision;
    } else if (latched_node_id->document_id != expected_document_id) {
        // A step over another document than the latched scroller's is left to the navigable hosting that scroller,
        // which reports the same step with its own document.
        return AsyncScrollResult::declined(Web::InputRoutingDecision::ScrollerInOtherDocument);
    }
    if (page_let_touchpad_step_through)
        m_wheel_gesture_routing.note_page_let_latched_gesture_through();

    auto scroll_chaining = is_first_step_of_gesture ? Compositing::ScrollChaining::ToScrollableAncestors : Compositing::ScrollChaining::None;
    auto outcome = perform_wheel_scroll_of_node(*latched_node_id, delta, wheel_delta_precision, scroll_gesture_phase, operation_tracking, viewport_rect, now, scroll_chaining, VisualViewportPanning::No);
    return {
        .enqueue_result = {
            .accepted = true,
            .operation_id = outcome.operation_id,
            .routing_decision = routing_decision,
            .routed_scroller = m_wheel_gesture_routing.latched_scroller(),
        },
        .frame_to_present = outcome.viewport_rect_to_present.map([](auto const& rect) { return PendingFrame::repainting_changes(rect); }),
    };
}

ContextState::AsyncScrollResult ContextState::smooth_scroll_to(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint destination_offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue)
{
    if (!m_has_async_scrolling_state)
        return {};

    if (operation_to_continue.has_value()) {
        auto running_animation = m_smooth_scroll_animations.find_if([&](auto const& animation) { return animation.operation_id == *operation_to_continue; });
        if (running_animation == m_smooth_scroll_animations.end())
            return {};
        retarget_user_scroll(*running_animation, destination_offset, MonotonicTime::now());
        m_async_scrolling_viewport_rect = viewport_rect;
        return {
            .enqueue_result = { true, running_animation->operation_id },
            .frame_to_present = PendingFrame::repainting_changes(viewport_rect),
        };
    }

    auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(stable_node_id);
    if (!node_id.has_value())
        return {};
    auto current_offset = m_async_scroll_tree.scroll_offset_for_node(*node_id, m_scroll_state_snapshot);
    if (!current_offset.has_value())
        return {};

    cancel_smooth_scroll(stable_node_id);

    // The main thread can apply instant input scrolls that this context has not learned about yet, so a scroll whose
    // presented offset already matches the destination starts from the main thread's offset to converge on it.
    auto start_offset = *current_offset;
    if (start_offset == destination_offset)
        start_offset = main_thread_offset;

    auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
    Compositing::SmoothScrollAnimation animation { start_offset, destination_offset, m_async_scroll_tree.device_pixels_per_css_pixel(), animation_kind };
    if (animation.duration().is_zero()) {
        m_async_scroll_update_outbox.complete_operation(operation_id);
        request_rendering_update();
        return {
            .enqueue_result = { true, operation_id },
            .frame_to_present = {},
        };
    }

    m_smooth_scroll_animations.append({
        .stable_node_id = stable_node_id,
        .operation_id = operation_id,
        .animation = move(animation),
        .started_at = MonotonicTime::now(),
        .is_user_scroll = initiator == Compositing::SmoothScrollInitiator::UserInput,
    });
    m_async_scrolling_viewport_rect = viewport_rect;
    return {
        .enqueue_result = { true, operation_id },
        .frame_to_present = PendingFrame::repainting_changes(viewport_rect),
    };
}

Optional<Gfx::FloatPoint> ContextState::in_flight_wheel_destination(Web::AsyncScrollNodeStableID stable_node_id) const
{
    for (auto const& animation : m_smooth_scroll_animations) {
        if (animation.stable_node_id == stable_node_id && animation.animation.kind() == Compositing::ScrollAnimationKind::Wheel)
            return animation.animation.destination_offset();
    }
    return {};
}

ContextState::AsyncScrollResult ContextState::smooth_wheel_scroll_by(Optional<Compositing::WheelScrollTarget> const& scroll_target, Gfx::IntRect viewport_rect, MonotonicTime now)
{
    if (!scroll_target.has_value()) {
        auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
        m_async_scroll_update_outbox.complete_operation(operation_id);
        return {
            .enqueue_result = { true, operation_id },
            .frame_to_present = {},
        };
    }

    for (auto& active_animation : m_smooth_scroll_animations) {
        if (active_animation.stable_node_id != scroll_target->stable_node_id)
            continue;
        if (active_animation.animation.kind() == Compositing::ScrollAnimationKind::Wheel) {
            retarget_user_scroll(active_animation, scroll_target->destination_offset, now);
            report_started_user_scroll(active_animation.operation_id, scroll_target->stable_node_id, scroll_target->start_offset, scroll_target->destination_offset, active_animation.animation.kind());
            m_async_scrolling_viewport_rect = viewport_rect;
            return {
                .enqueue_result = { true, active_animation.operation_id },
                .frame_to_present = PendingFrame::repainting_changes(viewport_rect),
            };
        }
        m_async_scroll_update_outbox.note_operation_taken_over_by_user_input(active_animation.operation_id);
        cancel_smooth_scroll(scroll_target->stable_node_id);
        break;
    }

    auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
    m_smooth_scroll_animations.append({
        .stable_node_id = scroll_target->stable_node_id,
        .operation_id = operation_id,
        .animation = Compositing::SmoothScrollAnimation { scroll_target->current_offset, scroll_target->destination_offset, m_async_scroll_tree.device_pixels_per_css_pixel(), Compositing::ScrollAnimationKind::Wheel },
        .started_at = now,
        .is_user_scroll = true,
    });
    report_started_user_scroll(operation_id, scroll_target->stable_node_id, scroll_target->start_offset, scroll_target->destination_offset, Compositing::ScrollAnimationKind::Wheel);
    m_async_scrolling_viewport_rect = viewport_rect;
    return {
        .enqueue_result = { true, operation_id },
        .frame_to_present = PendingFrame::repainting_changes(viewport_rect),
    };
}

void ContextState::retarget_user_scroll(ActiveSmoothScrollAnimation& running_animation, Gfx::FloatPoint destination_offset, MonotonicTime now)
{
    m_scroll_snap_controller.did_retarget_scroll(running_animation.stable_node_id, running_animation.operation_id);
    running_animation.animation.retarget(destination_offset, now - running_animation.started_at);
}

void ContextState::cancel_smooth_scroll(Web::AsyncScrollNodeStableID stable_node_id)
{
    retire_smooth_scroll_animation(stable_node_id);
    m_scroll_snap_controller.did_start_main_thread_scroll(stable_node_id);
}

void ContextState::retire_smooth_scroll_animation(Web::AsyncScrollNodeStableID stable_node_id)
{
    for (size_t index = 0; index < m_smooth_scroll_animations.size(); ++index) {
        auto const& smooth_scroll_animation = m_smooth_scroll_animations[index];
        if (smooth_scroll_animation.stable_node_id != stable_node_id)
            continue;
        if (m_scroll_snap_controller.is_snap_scroll(stable_node_id, smooth_scroll_animation.operation_id)) {
            Optional<Web::CSSPixelPoint> scroll_offset;
            if (auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(stable_node_id); node_id.has_value())
                scroll_offset = m_async_scroll_tree.css_scroll_offset_for_node(*node_id, m_scroll_state_snapshot);
            m_scroll_snap_controller.did_end_snap_scroll(stable_node_id, smooth_scroll_animation.operation_id, scroll_offset);
        }
        m_async_scroll_update_outbox.complete_operation(smooth_scroll_animation.operation_id);
        m_smooth_scroll_animations.remove(index);
        request_rendering_update();
        return;
    }
}

Optional<Gfx::IntRect> ContextState::advance_smooth_scroll_animations(MonotonicTime now)
{
    Vector<Compositing::AsyncScrollOffset> scroll_offsets;
    bool changed_scroll_offset = false;
    bool completed_operation = false;

    for (size_t index = 0; index < m_smooth_scroll_animations.size();) {
        auto& active_animation = m_smooth_scroll_animations[index];
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(active_animation.stable_node_id);
        if (!node_id.has_value()) {
            m_async_scroll_update_outbox.complete_operation(active_animation.operation_id);
            completed_operation = true;
            m_smooth_scroll_animations.remove(index);
            continue;
        }

        auto old_offset = m_async_scroll_tree.scroll_offset_for_node(*node_id, m_scroll_state_snapshot);
        if (!old_offset.has_value()) {
            m_async_scroll_update_outbox.complete_operation(active_animation.operation_id);
            completed_operation = true;
            m_smooth_scroll_animations.remove(index);
            continue;
        }

        auto sample = active_animation.animation.sample(now - active_animation.started_at);
        auto new_offset = m_async_scroll_tree.set_scroll_offset(*node_id, sample.offset, m_scene.current_visual_context_tree(), m_scroll_state_snapshot);
        VERIFY(new_offset.has_value());
        auto scroll_delta = *new_offset - *old_offset;
        if (!scroll_delta.is_zero()) {
            scroll_offsets.append({
                .stable_node_id = active_animation.stable_node_id,
                .compositor_scroll_offset = *new_offset,
                .unadopted_scroll_delta = scroll_delta,
                .last_relative_scroll_delta = {},
            });
            changed_scroll_offset = true;
            if (m_async_scroll_tree.scroll_node_is_viewport(*node_id))
                m_async_scrolling_viewport_rect.set_location(new_offset->to_type<int>());
        }

        if (sample.complete) {
            m_scroll_snap_controller.did_end_snap_scroll(active_animation.stable_node_id, active_animation.operation_id, m_async_scroll_tree.css_pixels_from_device_offset(*new_offset));
            m_async_scroll_update_outbox.complete_operation(active_animation.operation_id);
            completed_operation = true;
            m_smooth_scroll_animations.remove(index);
        } else {
            ++index;
        }
    }

    if (!scroll_offsets.is_empty()) {
        rebuild_wheel_hit_test_targets();
        store_pending_async_scroll_offsets(scroll_offsets);
    }
    if (changed_scroll_offset || completed_operation)
        request_rendering_update();
    if (changed_scroll_offset)
        return m_async_scrolling_viewport_rect;
    return {};
}

ContextState::ContextUpdateResult ContextState::handle_wheel_event(Web::MouseEvent const& event, Optional<MonotonicTime> now_for_testing)
{
    VERIFY(event.type == Web::MouseEvent::Type::MouseWheel);
    auto now = now_for_testing.value_or(MonotonicTime::now());
    m_scroll_fling_controller.note_wheel_event(event, now);
    return scroll_for_wheel_event(event, now);
}

static Gfx::FloatPoint wheel_event_position(Web::MouseEvent const& event)
{
    return {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };
}

Gfx::FloatPoint ContextState::wheel_event_delta_in_device_pixels(Web::MouseEvent const& event) const
{
    auto wheel_delta_x = event.wheel_delta_x;
    auto wheel_delta_y = event.wheel_delta_y;
    if (event.modifiers & Web::UIEvents::KeyModifier::Mod_Shift)
        swap(wheel_delta_x, wheel_delta_y);
    return Gfx::FloatPoint { static_cast<float>(wheel_delta_x), static_cast<float>(wheel_delta_y) }
        .scaled(static_cast<float>(m_async_scroll_tree.device_pixels_per_css_pixel()));
}

bool ContextState::step_continues_touchpad_gesture_the_page_let_through(Optional<Compositing::AsyncScrollNodeID> const& latched_node_id, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase) const
{
    return latched_node_id.has_value() && presents_to_client() && m_wheel_gesture_routing.page_let_latched_touchpad_gesture_through(wheel_delta_precision, scroll_gesture_phase);
}

void ContextState::note_wheel_event_from_ui(Web::MouseEvent const& event, Optional<MonotonicTime> now_for_testing)
{
    VERIFY(event.type == Web::MouseEvent::Type::MouseWheel);
    auto now = now_for_testing.value_or(MonotonicTime::now());
    auto phase = event.scroll_gesture_phase;

    // The end of the gesture before this one may never have arrived.
    if (m_wheel_gesture_routing.note_wheel_event_from_ui(event, wheel_event_position(event), m_async_scroll_tree, now))
        m_elastic_overscroll_controller.end_gesture(now);

    // A stretch is released with the velocity of the steps as they arrived from the UI, not as they were scrolled.
    // Fling steps are not timed here, which leaves them out of the velocity; no platform both flings and stretches.
    if (wheel_step_can_stretch_elastically(event.wheel_delta_precision, phase))
        m_elastic_overscroll_controller.note_step_from_ui(wheel_event_delta_in_device_pixels(event).scaled(1.0f / m_scene.visual_viewport_magnification()), now);
}

bool ContextState::start_scroll_fling_if_flicked(Web::MouseEvent const& gesture_end_event, Optional<MonotonicTime> now_for_testing)
{
    return m_scroll_fling_controller.start_if_flicked(gesture_end_event, now_for_testing.value_or(MonotonicTime::now()));
}

Optional<Web::MouseEvent> ContextState::interrupt_scroll_fling(Web::MouseEvent const& event, Optional<MonotonicTime> now_for_testing)
{
    return m_scroll_fling_controller.interrupt(event, now_for_testing.value_or(MonotonicTime::now()));
}

Optional<Web::MouseEvent> ContextState::take_scroll_fling_step(MonotonicTime frame_time)
{
    return m_scroll_fling_controller.take_step(frame_time);
}

Optional<Web::MouseEvent> ContextState::end_scroll_fling()
{
    return m_scroll_fling_controller.end();
}

ContextState::ContextUpdateResult ContextState::scroll_for_fling_step(Web::MouseEvent const& event, MonotonicTime now)
{
    return scroll_for_wheel_event(event, now);
}

void ContextState::end_scroll_fling_on_next_step()
{
    m_scroll_fling_controller.end_on_next_step();
}

ContextState::ContextUpdateResult ContextState::scroll_for_wheel_event(Web::MouseEvent const& event, MonotonicTime now)
{
    if (!presents_to_client())
        return ContextUpdateResult::declined(Web::InputRoutingDecision::ContextNotPresenting);

    auto position = wheel_event_position(event);
    // The end of a gesture lets go of the scroller it stretched even if the context has stopped taking wheel events.
    if (event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended)
        m_elastic_overscroll_controller.end_gesture(now);

    auto latch = resolve_wheel_scroll_latch(position, event.scroll_gesture_phase, event.modifiers, now);
    if (latch.nested_context_id.has_value())
        return { .nested_context_wheel_step = NestedContextWheelStep { .context_id = *latch.nested_context_id } };

    // The first step of a gesture over the content of a nested context goes to that context. A gesture whose first step
    // went elsewhere is not handed to a nested context that comes under the pointer later.
    auto nested_context_regions = Compositing::NestedContextRegions::LeaveToMainThread;
    bool may_hand_step_to_nested_context = !latch.node_id.has_value()
        && m_wheel_gesture_routing.is_first_wheel_event_from_ui_with_delta(event.id)
        && may_hand_first_wheel_steps_to_nested_contexts();
    if (may_hand_step_to_nested_context) {
        nested_context_regions = Compositing::NestedContextRegions::HandToNestedContext;
        if (auto hit = nested_context_for_wheel_at(position); hit.has_value())
            return { .nested_context_wheel_step = NestedContextWheelStep { .context_id = hit->child_context_id, .hit = hit } };
    }
    return scroll_for_wheel_event_in_this_context(event, latch.node_id, nested_context_regions, now);
}

ContextState::ContextUpdateResult ContextState::scroll_for_wheel_event_passed_on_by_nested_context(Web::MouseEvent const& event, MonotonicTime now)
{
    return scroll_for_wheel_event_in_this_context(event, {}, Compositing::NestedContextRegions::Ignore, now);
}

ContextState::ContextUpdateResult ContextState::scroll_for_wheel_event_in_this_context(Web::MouseEvent const& event, Optional<Compositing::AsyncScrollNodeID> latched_node_id, Compositing::NestedContextRegions nested_context_regions, MonotonicTime now)
{
    auto position = wheel_event_position(event);
    auto delta = wheel_event_delta_in_device_pixels(event);

    // A page without scroll nodes still has a visual viewport to pan once it is zoomed in.
    auto can_pan_zoomed_visual_viewport = m_wheel_gesture_routing.admission() == Compositing::WheelRoutingAdmission::NoScrollNode
        && m_scene.visual_viewport_scale().value_or(1.0f) > 1.0f;
    if (!m_wheel_gesture_routing.admits_async_wheel() && !can_pan_zoomed_visual_viewport
        && !step_continues_touchpad_gesture_the_page_let_through(latched_node_id, event.wheel_delta_precision, event.scroll_gesture_phase))
        return ContextUpdateResult::declined(Compositing::input_routing_decision_for_wheel_step_not_admitted_by(m_wheel_gesture_routing.admission()));

    m_scroll_snap_controller.note_gesture_phase(event.scroll_gesture_phase);

    if (event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && delta.is_zero()) {
        if (!snap_at_gesture_end(now).has_value())
            return ContextUpdateResult::declined(Web::InputRoutingDecision::GestureEndWithNothingToSnap);
        return {
            .accepted = true,
            .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
            .should_request_rendering_update = true,
            .routing_decision = Web::InputRoutingDecision::SnappedAtGestureEnd,
        };
    }

    // Whether a gesture scrolls here or on the main thread is decided by its first step; the steps of a latched
    // gesture are not held back by regions the cursor moves over later.
    bool is_first_step_of_gesture = !latched_node_id.has_value();
    if (is_first_step_of_gesture) {
        auto initial_scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(m_scene.visual_context_tree_for_compositing(), position, delta, {}, Compositing::WheelStepDispatch::NotYetDispatched, nested_context_regions);
        if (initial_scroll_target.is_blocked_by_a_region())
            return ContextUpdateResult::declined(Compositing::input_routing_decision_for_first_wheel_step(initial_scroll_target.outcome));
    }

    if (delta.is_zero()) {
        return {
            .accepted = true,
            .frame_to_present = {},
            .should_request_rendering_update = false,
            .routing_decision = Web::InputRoutingDecision::StepWithoutDelta,
            .routed_scroller = m_wheel_gesture_routing.latched_scroller(),
        };
    }

    // A gesture is latched before its first step pans the visual viewport, so that the scroller it belongs to is known
    // for as long as the gesture only pans.
    auto routing_decision = Web::InputRoutingDecision::ContinuedLatch;
    if (is_first_step_of_gesture) {
        auto latch_attempt = hit_test_and_latch_wheel_gesture(position, delta.scaled(1.0f / m_scene.visual_viewport_magnification()), event.wheel_delta_precision, event.scroll_gesture_phase, event.modifiers, now, {}, Compositing::WheelStepDispatch::NotYetDispatched, nested_context_regions);
        latched_node_id = latch_attempt.latched_node_id;
        routing_decision = latch_attempt.decision;
    }

    // A zoomed page still pans its visual viewport by a gesture that no scroller takes.
    if (!latched_node_id.has_value()) {
        if (pan_visual_viewport_by(delta).is_zero())
            return ContextUpdateResult::declined(routing_decision);
        return {
            .accepted = true,
            .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
            .should_request_rendering_update = true,
            .routing_decision = Web::InputRoutingDecision::PannedVisualViewport,
        };
    }

    auto scroll_chaining = is_first_step_of_gesture ? Compositing::ScrollChaining::ToScrollableAncestors : Compositing::ScrollChaining::None;
    auto outcome = perform_wheel_scroll_of_node(*latched_node_id, delta, event.wheel_delta_precision, event.scroll_gesture_phase, Compositing::AsyncScrollOperationTracking::No, m_async_scrolling_viewport_rect, now, scroll_chaining, VisualViewportPanning::BeforeScrolling);
    return {
        .accepted = true,
        .frame_to_present = outcome.viewport_rect_to_present.map([](auto const& rect) { return PendingFrame::repainting_changes(rect); }),
        .should_request_rendering_update = outcome.viewport_rect_to_present.has_value() && !outcome.only_elastic_overscroll_changed,
        .routing_decision = routing_decision,
        .routed_scroller = m_wheel_gesture_routing.latched_scroller(),
    };
}

bool ContextState::may_hand_first_wheel_steps_to_nested_contexts() const
{
    // A zoomed page pans its visual viewport before anything scrolls. This document's wheel event listeners never see
    // the wheel events over a document nested in it, whatever state they are in.
    return m_has_async_scrolling_state && m_scene.visual_viewport_scale().value_or(1.0f) == 1.0f;
}

Optional<Compositing::NestedContextWheelHit> ContextState::nested_context_for_wheel_at(Gfx::FloatPoint position)
{
    if (!m_has_async_scrolling_state)
        return {};
    return m_async_scroll_tree.nested_context_for_wheel_at(m_scene.visual_context_tree_for_compositing(), position);
}

void ContextState::latch_wheel_gesture_to_nested_context(Web::CompositorContextId nested_context_id, Optional<Web::AsyncScrollNodeStableID> scroller, Web::MouseEvent const& event, MonotonicTime now)
{
    m_wheel_gesture_routing.latch_to_nested_context(nested_context_id, scroller, wheel_event_position(event), event.scroll_gesture_phase, event.modifiers, now);
}

ContextState::WheelStepFromContainingContext ContextState::WheelStepFromContainingContext::for_wheel_event(Web::MouseEvent const& event)
{
    Gfx::FloatPoint delta_in_css_pixels { static_cast<float>(event.wheel_delta_x), static_cast<float>(event.wheel_delta_y) };
    if (event.modifiers & Web::UIEvents::KeyModifier::Mod_Shift)
        delta_in_css_pixels = { delta_in_css_pixels.y(), delta_in_css_pixels.x() };
    return {
        .position = {},
        .delta_in_css_pixels = delta_in_css_pixels,
        .wheel_delta_precision = event.wheel_delta_precision,
        .scroll_gesture_phase = event.scroll_gesture_phase,
        .modifiers = event.modifiers,
    };
}

ContextState::FirstWheelStepRoute ContextState::route_first_wheel_step_from_containing_context(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision wheel_delta_precision, Compositing::NestedContextRegions nested_context_regions)
{
    using Kind = FirstWheelStepRoute::Kind;
    auto admission = m_wheel_gesture_routing.admission();
    if (!m_has_async_scrolling_state || admission == Compositing::WheelRoutingAdmission::NoAsyncScrollingState)
        return { Kind::Declined, Web::InputRoutingDecision::NoAsyncScrollingState };

    // A document's wheel event listeners never see the wheel events over a document nested in it.
    auto const& visual_context_tree = m_scene.visual_context_tree_for_compositing();
    if (nested_context_regions == Compositing::NestedContextRegions::HandToNestedContext) {
        if (auto hit = m_async_scroll_tree.nested_context_for_wheel_at(visual_context_tree, position); hit.has_value())
            return { .kind = Kind::NestedContext, .decision = Web::InputRoutingDecision::HandedToNestedContext, .nested_context_hit = hit };
    }
    if (first_is_one_of(admission, Compositing::WheelRoutingAdmission::BlockingWheelEventListeners, Compositing::WheelRoutingAdmission::StaleWheelEventListeners))
        return { Kind::Declined, Compositing::input_routing_decision_for_wheel_step_not_admitted_by(admission) };

    auto delta = delta_in_css_pixels.scaled(static_cast<float>(m_async_scroll_tree.device_pixels_per_css_pixel()));
    Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> in_flight_destination_for;
    if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete)
        in_flight_destination_for = [this](auto stable_node_id) { return in_flight_wheel_destination(stable_node_id); };
    auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(visual_context_tree, position, delta, in_flight_destination_for, Compositing::WheelStepDispatch::NotYetDispatched, nested_context_regions);
    auto scroller_of = [&](Optional<Compositing::AsyncScrollNodeID> node_id) {
        return node_id.map([&](auto id) { return m_async_scroll_tree.scroll_node_for_id(id)->stable_node_id; });
    };
    if (scroll_target.outcome == Compositing::WheelHitTestOutcome::FoundScrollTarget)
        return { .kind = Kind::ScrollNode, .decision = Web::InputRoutingDecision::Latched, .node_id = scroll_target.node_id, .scroller = scroller_of(scroll_target.node_id) };
    if (scroll_target.outcome != Compositing::WheelHitTestOutcome::NoScrollTarget)
        return { Kind::Declined, Compositing::input_routing_decision_for_first_wheel_step(scroll_target.outcome) };

    // https://drafts.csswg.org/css-overscroll-1/#scroll-chaining-and-boundary-default-actions
    // A nested context does not stretch at its edge. It passes the step on to the document around it unless a scroller
    // keeps it, the viewport included.
    bool viewport_keeps_the_scroll = m_async_scroll_tree.viewport_keeps_scrolls_along_main_axis_of(delta);
    if (!scroll_target.node_keeping_the_scroll.has_value() && !viewport_keeps_the_scroll)
        return { Kind::PassedOn, Web::InputRoutingDecision::NestedContextPassedStepOn };
    auto node_keeping_the_scroll = scroll_target.node_keeping_the_scroll.has_value() ? scroll_target.node_keeping_the_scroll : m_async_scroll_tree.viewport_scroll_node_id();
    return { .kind = Kind::Kept, .decision = Web::InputRoutingDecision::ScrollKeptByOverscrollBehavior, .node_id = node_keeping_the_scroll, .scroller = scroller_of(node_keeping_the_scroll) };
}

ContextState::ContextUpdateResult ContextState::take_first_wheel_step_from_containing_context(FirstWheelStepRoute const& route, WheelStepFromContainingContext const& step, MonotonicTime now)
{
    VERIFY(first_is_one_of(route.kind, FirstWheelStepRoute::Kind::ScrollNode, FirstWheelStepRoute::Kind::Kept));
    if (route.kind == FirstWheelStepRoute::Kind::Kept) {
        if (route.scroller.has_value())
            m_wheel_gesture_routing.latch(*route.scroller, step.position, step.scroll_gesture_phase, step.modifiers, now);
        else
            m_wheel_gesture_routing.latch_keeping_steps(step.position, step.scroll_gesture_phase, step.modifiers, now);
        return { .accepted = true, .routing_decision = route.decision, .routed_scroller = route.scroller };
    }
    m_wheel_gesture_routing.latch(*route.scroller, step.position, step.scroll_gesture_phase, step.modifiers, now);
    return take_wheel_step_from_containing_context(*route.node_id, route.decision, step, Compositing::ScrollChaining::ToScrollableAncestors, now);
}

ContextState::ContextUpdateResult ContextState::take_latched_wheel_step_from_containing_context(WheelStepFromContainingContext const& step, MonotonicTime now)
{
    if (!m_has_async_scrolling_state)
        return ContextUpdateResult::declined(Web::InputRoutingDecision::NoAsyncScrollingState);
    // A blocking listener added during the gesture holds its later steps back until a display list shows where it is.
    auto admission = m_wheel_gesture_routing.admission();
    if (first_is_one_of(admission, Compositing::WheelRoutingAdmission::BlockingWheelEventListeners, Compositing::WheelRoutingAdmission::StaleWheelEventListeners))
        return ContextUpdateResult::declined(Compositing::input_routing_decision_for_wheel_step_not_admitted_by(admission));

    auto latch = m_wheel_gesture_routing.resolve_latch_continued_by_containing_context(m_async_scroll_tree, m_scene.current_visual_context_tree(), step.scroll_gesture_phase, now);
    if (latch.keeps_steps)
        return { .accepted = true, .routing_decision = Web::InputRoutingDecision::ScrollKeptByOverscrollBehavior };
    if (!latch.node_id.has_value())
        return ContextUpdateResult::declined(Web::InputRoutingDecision::NestedContextUnavailable);

    if (step.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && step.delta_in_css_pixels.is_zero()) {
        m_scroll_snap_controller.note_gesture_phase(step.scroll_gesture_phase);
        if (!snap_at_gesture_end(now).has_value())
            return ContextUpdateResult::declined(Web::InputRoutingDecision::GestureEndWithNothingToSnap);
        return {
            .accepted = true,
            .frame_to_present = PendingFrame::repainting_changes(m_async_scrolling_viewport_rect),
            .should_request_rendering_update = true,
            .routing_decision = Web::InputRoutingDecision::SnappedAtGestureEnd,
            .routed_scroller = m_wheel_gesture_routing.latched_scroller(),
        };
    }
    return take_wheel_step_from_containing_context(*latch.node_id, Web::InputRoutingDecision::ContinuedLatch, step, Compositing::ScrollChaining::None, now);
}

ContextState::ContextUpdateResult ContextState::take_wheel_step_from_containing_context(Compositing::AsyncScrollNodeID node_id, Web::InputRoutingDecision decision, WheelStepFromContainingContext const& step, Compositing::ScrollChaining scroll_chaining, MonotonicTime now)
{
    m_scroll_snap_controller.note_gesture_phase(step.scroll_gesture_phase);
    auto delta = step.delta_in_css_pixels.scaled(static_cast<float>(m_async_scroll_tree.device_pixels_per_css_pixel()));
    auto outcome = perform_wheel_scroll_of_node(node_id, delta, step.wheel_delta_precision, step.scroll_gesture_phase, Compositing::AsyncScrollOperationTracking::No, m_async_scrolling_viewport_rect, now, scroll_chaining, VisualViewportPanning::No);
    return {
        .accepted = true,
        .frame_to_present = outcome.viewport_rect_to_present.map([](auto const& rect) { return PendingFrame::repainting_changes(rect); }),
        .should_request_rendering_update = outcome.viewport_rect_to_present.has_value() && !outcome.only_elastic_overscroll_changed,
        .routing_decision = decision,
        .routed_scroller = m_wheel_gesture_routing.latched_scroller(),
    };
}

bool ContextState::wheel_step_can_stretch_elastically(Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase) const
{
    return presents_to_client() && m_elastic_overscroll_controller.step_can_stretch(wheel_delta_precision, scroll_gesture_phase);
}

bool ContextState::pull_elastic_overscroll_by(Compositing::AsyncScrollNode const& node, Gfx::FloatPoint step_delta, Gfx::FloatPoint delta_the_scroller_did_not_scroll_by)
{
    auto scroll_offset = m_async_scroll_tree.scroll_offset_for_node(node.node_id, m_scroll_state_snapshot);
    if (!scroll_offset.has_value())
        return false;
    return m_elastic_overscroll_controller.pull_by(node, *scroll_offset, step_delta, delta_the_scroller_did_not_scroll_by, m_scene.visual_viewport_magnification());
}

void ContextState::set_scrolls_elastically(bool scrolls_elastically)
{
    m_elastic_overscroll_controller.set_enabled(scrolls_elastically);
}

Optional<Gfx::IntRect> ContextState::advance_elastic_overscroll_rebounds(MonotonicTime frame_time)
{
    if (!m_elastic_overscroll_controller.advance_rebounds(frame_time))
        return {};
    return m_async_scrolling_viewport_rect;
}

Compositing::FirstWheelStepRouteForTesting ContextState::route_first_wheel_step_for_testing(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels, Compositing::NestedContextRegions nested_context_regions)
{
    auto document_id = m_async_scroll_tree.document_id();
    if (!presents_to_client())
        return { { Web::InputRoutingDecision::ContextNotPresenting }, document_id };
    if (!m_scene.display_list() || !m_scene.visual_context_tree().has_value())
        return { { Web::InputRoutingDecision::NoAsyncScrollingState }, document_id };
    auto delta = delta_in_css_pixels.scaled(static_cast<float>(m_async_scroll_tree.device_pixels_per_css_pixel()) / m_scene.visual_viewport_magnification());
    return { m_async_scroll_tree.route_first_wheel_step(m_wheel_gesture_routing.admission(), m_scene.visual_context_tree_for_compositing(), position, delta, nested_context_regions), document_id };
}

String ContextState::scroll_state_for_testing(Web::AsyncScrollNodeStableID stretched_scroller) const
{
    auto const& display_list = m_scene.display_list();
    auto scroll_state = Compositing::async_scrolling_state_to_json(display_list ? Compositing::async_scrolling_state_from_display_list(*display_list) : Compositing::AsyncScrollingState {});

    auto admission = m_wheel_gesture_routing.admission();
    auto admission_name = admission == Compositing::WheelRoutingAdmission::Accepted
        ? "accepted"sv
        : Web::input_routing_decision_name(Compositing::input_routing_decision_for_wheel_step_not_admitted_by(admission));
    scroll_state.set("wheelRoutingAdmission"sv, admission_name);

    auto stretch = m_elastic_overscroll_controller.stretch_of(stretched_scroller).scaled(1.0f / static_cast<float>(m_async_scroll_tree.device_pixels_per_css_pixel()));
    JsonObject stretch_object;
    stretch_object.set("x"sv, stretch.x());
    stretch_object.set("y"sv, stretch.y());
    scroll_state.set("stretch"sv, move(stretch_object));
    return scroll_state.serialized();
}

Optional<Compositing::ScrollStateSnapshot> ContextState::scroll_state_snapshot_stretched_by_elastic_overscrolls() const
{
    return m_elastic_overscroll_controller.stretched_scroll_state_snapshot(m_scroll_state_snapshot, m_async_scroll_tree);
}

Compositing::PendingAsyncScrollUpdates ContextState::take_pending_async_scroll_updates()
{
    auto updates = m_async_scroll_update_outbox.take_updates(user_scroll_gesture_in_progress());
    updates.document_id = m_async_scroll_tree.document_id();
    return updates;
}

bool ContextState::has_pending_async_scroll_updates() const
{
    return m_async_scroll_update_outbox.has_updates(user_scroll_gesture_in_progress());
}

void ContextState::viewport_size_updated(Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    m_scene.forget_whether_animated_content_may_affect_viewport();
    if (m_presenter.viewport_size() != viewport_size)
        m_keyboard_scroll_controller.forget_target();
    auto is_page_presentation_context = m_page_id.has_value() && !m_parent_context_id.has_value();
    m_presenter.set_viewport_size(viewport_size, device_pixel_ratio, is_page_presentation_context ? window_resize_in_progress : Compositing::WindowResizingInProgress::No);
}

bool ContextState::set_ui_overlays(Compositing::UIOverlays overlays)
{
    if (m_presenter.ui_overlays() == overlays)
        return false;

    if (overlays.paused_debugger.has_value())
        end_keyboard_scroll_gesture();
    m_presenter.set_ui_overlays(move(overlays));
    return true;
}

bool ContextState::set_display_metadata(Optional<u64> display_id, double refresh_rate)
{
    m_display_id = display_id;
    m_display_refresh_rate = refresh_rate;
    return m_presenter.pending_present_frame_scheduled();
}

bool ContextState::set_visibility(Compositing::ContextVisibility visibility)
{
    if (m_visibility == visibility)
        return false;
    m_visibility = visibility;
    if (visibility == Compositing::ContextVisibility::Hidden) {
        end_keyboard_scroll_gesture();
        m_elastic_overscroll_controller.forget();
    }
    return true;
}

Vector<Web::CompositorScrollOffset> ContextState::scroll_offsets() const
{
    auto scroll_nodes = m_async_scroll_tree.scroll_nodes();
    Vector<Web::CompositorScrollOffset> scroll_offsets;
    scroll_offsets.ensure_capacity(scroll_nodes.size());
    for (auto const& node : scroll_nodes) {
        if (auto offset = m_async_scroll_tree.css_scroll_offset_for_node(node.node_id, m_scroll_state_snapshot); offset.has_value())
            scroll_offsets.unchecked_append({ node.stable_node_id, *offset });
    }
    return scroll_offsets;
}

bool ContextState::has_pending_present_frame_scheduled_on(Optional<u64> display_id) const
{
    return m_presenter.pending_present_frame_scheduled() && m_display_id == display_id;
}

ContextPresenter::FrameContent ContextState::frame_content(Compositing::ScrollStateSnapshot const& presented_scroll_state_snapshot, CompositedContextResolver const* composited_context_resolver) const
{
    return {
        .presented_scroll_state_snapshot = presented_scroll_state_snapshot,
        .scrollbar_controller = m_scrollbar_controller,
        .viewport_scroll_node_index = m_async_scroll_tree.viewport_scroll_node_id().map([](auto const& node_id) { return node_id.scroll_node_index; }),
        .composited_context_resolver = composited_context_resolver,
    };
}

// Each frame shows the scrollbars as of when it is painted. An elastic overscroll moves the presented scroll offsets
// only, and shows the scrollbars of the stretched scroller as a scroll does. Returns the damage of the scrollbars that
// the display list does not damage itself.
Gfx::IntRect ContextState::update_scrollbar_visibility()
{
    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    auto const* visual_context_tree = m_scene.visual_context_tree().has_value() ? &m_scene.visual_context_tree_for_compositing() : nullptr;
    auto now = MonotonicTime::now();
    auto damage_rect = m_scrollbar_controller.update_visibility(visual_context_tree, presented_scroll_state_snapshot, now);
    report_scrollbar_visibility_changes();

    // A hovered scrollbar starts to grow here, which needs display ticks before anything else asks for a frame.
    if (m_request_animation_frames && m_scrollbar_controller.fades_need_frame(now))
        m_request_animation_frames();

    // The fade out runs on display ticks, which the wait before it does not need.
    auto fade_out_time = m_scrollbar_controller.next_fade_out_time();
    if (!fade_out_time.has_value() || !m_request_animation_frames) {
        if (m_scrollbar_fade_out_timer)
            m_scrollbar_fade_out_timer->stop();
        return damage_rect;
    }
    if (!m_scrollbar_fade_out_timer) {
        m_scrollbar_fade_out_timer = Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
            m_request_animation_frames();
        });
    }
    auto delay = max(AK::Duration::zero(), *fade_out_time - now);
    m_scrollbar_fade_out_timer->restart(delay);
    return damage_rect;
}

// NB: This goes on the connection the compositor forwards input on, so the main thread knows which scrollbars have
//     faded out before it hit tests any later press.
void ContextState::report_scrollbar_visibility_changes()
{
    auto changes = m_scrollbar_controller.take_visibility_changes();
    if (!changes.is_empty())
        m_web_content_client.overlay_scrollbar_visibilities_changed(changes);
}

Optional<PendingFrame> ContextState::advance_scrollbar_fades(MonotonicTime frame_time)
{
    if (!m_scene.visual_context_tree().has_value())
        return {};
    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    auto damage_rect = m_scrollbar_controller.advance_fades(m_scene.visual_context_tree_for_compositing(), presented_scroll_state_snapshot, frame_time);
    report_scrollbar_visibility_changes();
    auto viewport_rect = m_presenter.viewport_rect_for_ui_overlay();
    if (damage_rect.is_empty() || !viewport_rect.has_value())
        return {};
    return PendingFrame { *viewport_rect, damage_rect };
}

Optional<PreparedFrame> ContextState::prepare_frame(DisplayListPlayerSkia& display_list_player, PendingFrame pending_frame, CompositedContextResolver const* composited_context_resolver)
{
    pending_frame.forced_damage_rect.unite(update_scrollbar_visibility());
    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    return m_presenter.prepare_frame(display_list_player, pending_frame, frame_content(presented_scroll_state_snapshot, composited_context_resolver));
}

bool ContextState::present_synchronously(DisplayListPlayerSkia& display_list_player, CompositedContextResolver const* composited_context_resolver)
{
    update_scrollbar_visibility();
    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    return m_presenter.present_synchronously(display_list_player, frame_content(presented_scroll_state_snapshot, composited_context_resolver));
}

Optional<PendingFrame> ContextState::paint_screenshot(DisplayListPlayerSkia& display_list_player, Gfx::ShareableBitmap& target_bitmap, CompositedContextResolver const* composited_context_resolver)
{
    VERIFY(can_paint_screenshot(target_bitmap));

    // Screenshot requests can arrive between display ticks, so sample animations and scrollbar visibility at capture
    // time.
    auto capture_time = MonotonicTime::now();
    advance_visual_animations(capture_time);
    update_scrollbar_visibility();
    auto scrollbar_fade_frame = advance_scrollbar_fades(capture_time);

    auto stretched_scroll_state_snapshot = scroll_state_snapshot_stretched_by_elastic_overscrolls();
    auto const& presented_scroll_state_snapshot = stretched_scroll_state_snapshot.has_value() ? *stretched_scroll_state_snapshot : m_scroll_state_snapshot;
    m_presenter.paint_screenshot(display_list_player, target_bitmap, frame_content(presented_scroll_state_snapshot, composited_context_resolver));
    return scrollbar_fade_frame;
}

bool ContextState::can_paint_screenshot(Gfx::ShareableBitmap& target_bitmap) const
{
    return m_scene.display_list() && target_bitmap.is_valid() && target_bitmap.bitmap();
}

Optional<ContextState::VisualViewportScrollDelta> ContextState::apply_visual_viewport_scroll_delta(Gfx::FloatPoint delta)
{
    if (delta.is_zero())
        return {};
    if (!m_scene.visual_context_tree().has_value())
        return {};

    // A page whose viewport cannot be scrolled records no viewport scroll node, and its viewport stays where it is.
    Web::AsyncScrollNodeStableID viewport_stable_id;
    Gfx::FloatPoint viewport_scroll_offset;
    if (auto viewport_node_id = m_async_scroll_tree.viewport_scroll_node_id(); viewport_node_id.has_value()) {
        auto scroll_offset = m_async_scroll_tree.scroll_offset_for_node(*viewport_node_id, m_scroll_state_snapshot);
        if (!scroll_offset.has_value())
            return {};
        viewport_stable_id = viewport_stable_id_from(*viewport_node_id);
        viewport_scroll_offset = *scroll_offset;
    } else if (auto document_id = m_async_scroll_tree.document_id(); document_id.has_value()) {
        viewport_stable_id = { .node_id = *document_id, .kind = Web::AsyncScrollNodeKind::Viewport };
        viewport_scroll_offset = m_async_scrolling_viewport_rect.location().to_type<float>();
    } else {
        return {};
    }

    auto transform = m_scene.visual_viewport_transform();
    auto scale = transform.matrix[0, 0];
    if (scale <= 1.0f)
        return {};

    auto min_x = -static_cast<float>(m_async_scrolling_viewport_rect.width()) * (scale - 1.0f);
    auto min_y = -static_cast<float>(m_async_scrolling_viewport_rect.height()) * (scale - 1.0f);

    auto old_x = transform.matrix[0, 3];
    auto old_y = transform.matrix[1, 3];
    auto new_x = clamp(old_x - delta.x(), min_x, 0.0f);
    auto new_y = clamp(old_y - delta.y(), min_y, 0.0f);
    Gfx::FloatPoint consumed_delta { old_x - new_x, old_y - new_y };
    if (consumed_delta.is_zero())
        return {};

    transform.matrix[0, 3] = new_x;
    transform.matrix[1, 3] = new_y;
    m_scene.set_async_visual_viewport_transform(transform);
    rebuild_wheel_hit_test_targets();

    return VisualViewportScrollDelta {
        .scroll_offset = {
            .stable_node_id = viewport_stable_id,
            .compositor_scroll_offset = viewport_scroll_offset,
            .unadopted_scroll_delta = consumed_delta.scaled(1.0f / scale),
            .last_relative_scroll_delta = {},
        },
        .consumed_delta = consumed_delta,
    };
}

Gfx::FloatPoint ContextState::pan_visual_viewport_by(Gfx::FloatPoint delta_in_screen_pixels)
{
    auto visual_viewport_scroll_delta = apply_visual_viewport_scroll_delta(delta_in_screen_pixels);
    if (!visual_viewport_scroll_delta.has_value())
        return {};
    store_pending_async_scroll_offsets({ &visual_viewport_scroll_delta->scroll_offset, 1 });
    return visual_viewport_scroll_delta->consumed_delta;
}

Optional<Gfx::FloatPoint> ContextState::reapply_unreconciled_async_scroll_offsets()
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& unreconciled : m_async_scroll_update_outbox.unreconciled_scroll_offsets()) {
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(unreconciled.stable_node_id);
        if (!node_id.has_value())
            continue;
        auto reconciled_scroll_offset = m_async_scroll_tree.set_scroll_offset(
            *node_id,
            unreconciled.compositor_scroll_offset,
            m_scene.current_visual_context_tree(),
            m_scroll_state_snapshot);
        if (reconciled_scroll_offset.has_value() && m_async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = *reconciled_scroll_offset;
    }
    return viewport_scroll_offset;
}

void ContextState::store_pending_async_scroll_offsets(
    ReadonlySpan<Compositing::AsyncScrollOffset> scroll_offsets,
    Optional<Compositing::AsyncScrollOperationID> operation_id)
{
    m_scene.forget_whether_animated_content_may_affect_viewport();
    m_async_scroll_update_outbox.record_scroll_offsets(scroll_offsets);
    if (operation_id.has_value())
        m_async_scroll_update_outbox.complete_operation(*operation_id);
}

WheelGestureRouting::ResolvedLatch ContextState::resolve_wheel_scroll_latch(Gfx::FloatPoint position, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, MonotonicTime now)
{
    // Only a hit test takes a latch, and it needs the tree of an installed display list.
    if (!m_wheel_gesture_routing.has_latch())
        return {};
    return m_wheel_gesture_routing.resolve_latch(m_async_scroll_tree, m_scene.current_visual_context_tree(), position, scroll_gesture_phase, modifiers, now);
}

ContextState::WheelGestureLatchAttempt ContextState::hit_test_and_latch_wheel_gesture(Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, MonotonicTime now, Optional<Web::UniqueNodeID> expected_document_id, Compositing::WheelStepDispatch wheel_step_dispatch, Compositing::NestedContextRegions nested_context_regions)
{
    Function<Optional<Gfx::FloatPoint>(Web::AsyncScrollNodeStableID)> in_flight_destination_for;
    if (wheel_delta_precision == Web::WheelDeltaPrecision::Discrete)
        in_flight_destination_for = [this](auto stable_node_id) { return in_flight_wheel_destination(stable_node_id); };
    auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(m_scene.visual_context_tree_for_compositing(), position, delta, in_flight_destination_for, wheel_step_dispatch, nested_context_regions);
    if (scroll_target.is_blocked_by_a_region())
        return { Compositing::input_routing_decision_for_first_wheel_step(scroll_target.outcome) };

    auto node_id_to_latch = scroll_target.node_id;
    auto decision = Web::InputRoutingDecision::Latched;
    // Fingers that start to pull where every scroller under them is already at its edge stretch the outermost one. A
    // context nested in another one turns the gesture away instead, which passes it on to the document around it.
    bool fingers_start_a_gesture_at_an_edge = !node_id_to_latch.has_value() && scroll_target.innermost_scroll_node_id.has_value() && !delta.is_zero()
        && scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing && wheel_step_can_stretch_elastically(wheel_delta_precision, scroll_gesture_phase);
    if (fingers_start_a_gesture_at_an_edge) {
        node_id_to_latch = m_async_scroll_tree.outermost_scroll_node_wheel_scrollable_along_main_axis_of(*scroll_target.innermost_scroll_node_id, delta);
        decision = Web::InputRoutingDecision::LatchedOutermostScrollerToStretch;
    }

    if (!node_id_to_latch.has_value())
        return { Compositing::input_routing_decision_for_first_wheel_step(scroll_target.outcome) };
    if (expected_document_id.has_value() && node_id_to_latch->document_id != *expected_document_id)
        return { Web::InputRoutingDecision::ScrollerInOtherDocument };
    auto const* node = m_async_scroll_tree.scroll_node_for_id(*node_id_to_latch);
    VERIFY(node);
    m_wheel_gesture_routing.latch(node->stable_node_id, position, scroll_gesture_phase, modifiers, now);
    return { decision, node_id_to_latch };
}

void ContextState::report_started_user_scroll(Compositing::AsyncScrollOperationID operation_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint step_start_offset, Gfx::FloatPoint destination_offset, Compositing::ScrollAnimationKind animation_kind, bool is_absolute_scroll)
{
    auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(stable_node_id);
    VERIFY(node_id.has_value());
    auto current_offset = m_async_scroll_tree.css_scroll_offset_for_node(*node_id, m_scroll_state_snapshot);
    VERIFY(current_offset.has_value());
    auto destination = m_async_scroll_tree.css_pixels_from_device_offset(destination_offset);
    m_async_scroll_update_outbox.record_started_user_scroll({
        .stable_node_id = stable_node_id,
        .operation_id = operation_id,
        .initial_scroll_offset = *current_offset,
        .unsnapped_scroll_destination = destination,
        .relative_scroll_delta = destination - m_async_scroll_tree.css_pixels_from_device_offset(step_start_offset),
        .selection = { .position = destination },
        .settles_gesture = false,
        .is_absolute_scroll = is_absolute_scroll,
        .animation_kind = animation_kind,
    });
}

void ContextState::cancel_smooth_scroll_taken_over_by_user_input(Compositing::AsyncScrollNodeID node_id)
{
    for (auto const& smooth_scroll_animation : m_smooth_scroll_animations) {
        auto animated_node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(smooth_scroll_animation.stable_node_id);
        if (animated_node_id != node_id)
            continue;
        m_async_scroll_update_outbox.note_operation_taken_over_by_user_input(smooth_scroll_animation.operation_id);
        cancel_smooth_scroll(smooth_scroll_animation.stable_node_id);
        return;
    }
}

void ContextState::note_user_scroll_gesture_end_if_drag_ended(bool was_dragging_scrollbar)
{
    if (was_dragging_scrollbar && !m_scrollbar_controller.has_captured_scrollbar())
        m_async_scroll_update_outbox.note_user_scroll_gesture_ended();
}

bool ContextState::user_scroll_gesture_in_progress() const
{
    return m_scrollbar_controller.has_captured_scrollbar()
        || m_keyboard_scroll_controller.has_held_keys()
        || m_scroll_snap_controller.has_gesture_awaiting_input();
}

void ContextState::schedule_end_of_scroll_step_gestures(MonotonicTime now)
{
    auto deadline = m_scroll_snap_controller.earliest_gesture_input_deadline();
    if (!deadline.has_value()) {
        if (m_scroll_step_gesture_input_timer)
            m_scroll_step_gesture_input_timer->stop();
        return;
    }
    if (!m_scroll_step_gesture_input_timer) {
        m_scroll_step_gesture_input_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [this] {
            end_scroll_step_gestures_whose_input_ran_out(MonotonicTime::now());
        });
    }
    m_scroll_step_gesture_input_timer->restart(max(*deadline - now, AK::Duration::zero()));
}

void ContextState::end_scroll_step_gestures_whose_input_ran_out(MonotonicTime now)
{
    if (m_scroll_snap_controller.end_gestures_whose_input_ran_out(now)) {
        m_async_scroll_update_outbox.note_user_scroll_gesture_ended();
        request_rendering_update();
    }
    schedule_end_of_scroll_step_gestures(now);
}

Optional<PendingFrame> ContextState::start_scrollbar_press_scroll(Compositing::AsyncScrollNode const& node, Gfx::FloatPoint old_scroll_offset, Gfx::FloatPoint destination, bool is_absolute_scroll)
{
    auto operation_id = m_async_scroll_update_outbox.allocate_operation_id();
    m_smooth_scroll_animations.append({
        .stable_node_id = node.stable_node_id,
        .operation_id = operation_id,
        .animation = Compositing::SmoothScrollAnimation { old_scroll_offset, destination, m_async_scroll_tree.device_pixels_per_css_pixel() },
        .started_at = MonotonicTime::now(),
        .is_user_scroll = true,
    });
    report_started_user_scroll(operation_id, node.stable_node_id, old_scroll_offset, destination, Compositing::ScrollAnimationKind::SmoothScroll, is_absolute_scroll);
    m_scrollbar_press_scroll_operation = operation_id;
    return PendingFrame::repainting_changes(m_async_scrolling_viewport_rect);
}

// https://source.chromium.org/chromium/chromium/src/+/main:cc/input/scroll_utils.cc
// INTEROP: A page is most of the scrollport, keeping as much of the last page in view as Chromium does.
static float scrollbar_page_step(float scrollport_length, float device_pixels_per_css_pixel)
{
    static constexpr float min_fraction_to_step_when_paging = 0.875f;
    static constexpr float max_overlap_between_pages = 40;
    return max(1.0f, max(scrollport_length * min_fraction_to_step_when_paging, scrollport_length - max_overlap_between_pages * device_pixels_per_css_pixel));
}

// Pages the scroller of the captured scrollbar once toward the pointer, from where a page scroll in flight goes, unless
// the thumb has reached the pointer there.
Optional<PendingFrame> ContextState::page_captured_scrollbar()
{
    auto scrollbar_index = m_scrollbar_controller.captured_scrollbar_index();
    if (!scrollbar_index.has_value() || !m_scrollbar_paging.has_value() || m_scrollbar_paging->direction == 0)
        return {};
    auto const& scrollbar = m_scrollbar_controller.scrollbars()[*scrollbar_index];
    auto const* node = m_async_scroll_tree.scroll_node_for_id(scrollbar.scroll_node_id);
    auto old_scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, m_scroll_state_snapshot);
    if (!node || !old_scroll_offset.has_value())
        return {};

    auto* running_animation = running_scrollbar_press_scroll();
    auto step_start = running_animation ? running_animation->animation.destination_offset() : *old_scroll_offset;
    if (m_scrollbar_controller.paging_direction(*scrollbar_index, m_scrollbar_paging->primary_position, step_start) != m_scrollbar_paging->direction)
        return {};

    auto orientation = scrollbar.vertical ? Gfx::Orientation::Vertical : Gfx::Orientation::Horizontal;
    auto step = scrollbar_page_step(static_cast<float>(node->scrollport_rect.primary_size_for_orientation(orientation)), m_async_scroll_tree.device_pixels_per_css_pixel());
    auto destination = step_start;
    destination.set_primary_offset_for_orientation(orientation, step_start.primary_offset_for_orientation(orientation) + static_cast<float>(m_scrollbar_paging->direction) * step);
    destination = m_async_scroll_tree.clamped_scroll_offset_for_node(scrollbar.scroll_node_id, destination);
    if (destination == step_start)
        return {};

    if (running_animation) {
        retarget_user_scroll(*running_animation, destination, MonotonicTime::now());
        report_started_user_scroll(running_animation->operation_id, node->stable_node_id, step_start, destination, Compositing::ScrollAnimationKind::SmoothScroll, false);
        return PendingFrame::repainting_changes(m_async_scrolling_viewport_rect);
    }
    cancel_smooth_scroll_taken_over_by_user_input(scrollbar.scroll_node_id);
    return start_scrollbar_press_scroll(*node, *old_scroll_offset, destination, false);
}

ContextState::ActiveSmoothScrollAnimation* ContextState::running_scrollbar_press_scroll()
{
    if (!m_scrollbar_press_scroll_operation.has_value())
        return nullptr;
    auto animation = m_smooth_scroll_animations.first_matching([&](auto const& animation) { return animation.operation_id == *m_scrollbar_press_scroll_operation; });
    if (!animation.has_value()) {
        m_scrollbar_press_scroll_operation.clear();
        return nullptr;
    }
    return &animation.value();
}

void ContextState::stop_scrollbar_paging()
{
    m_scrollbar_paging.clear();
    if (m_scrollbar_paging_timer)
        m_scrollbar_paging_timer->stop();
}

Optional<PendingFrame> ContextState::apply_scrollbar_drag(ScrollbarController::Drag const& drag)
{
    auto dragged_to = m_scrollbar_controller.scroll_offset_for_drag(m_async_scroll_tree, m_scroll_state_snapshot, drag);
    if (!dragged_to.has_value())
        return {};

    // A scrollbar scrolls its own scroller to where the thumb was dragged, and never hands any of that to an ancestor.
    auto node_id = dragged_to->scroll_node_id;
    auto const* node = m_async_scroll_tree.scroll_node_for_id(node_id);
    auto old_scroll_offset = m_async_scroll_tree.scroll_offset_for_node(node_id, m_scroll_state_snapshot);
    if (!node || !old_scroll_offset.has_value())
        return {};
    auto destination = m_async_scroll_tree.clamped_scroll_offset_for_node(node_id, dragged_to->scroll_offset);

    // A press beside the thumb scrolls there smoothly, as a page step does. Dragging on before that scroll has ended
    // moves where it goes.
    if (auto* running_animation = running_scrollbar_press_scroll()) {
        if (running_animation->animation.destination_offset() == destination)
            return {};
        retarget_user_scroll(*running_animation, destination, MonotonicTime::now());
        report_started_user_scroll(running_animation->operation_id, node->stable_node_id, *old_scroll_offset, destination, Compositing::ScrollAnimationKind::SmoothScroll, true);
        return PendingFrame::repainting_changes(m_async_scrolling_viewport_rect);
    }

    if (destination == *old_scroll_offset)
        return {};

    cancel_smooth_scroll_taken_over_by_user_input(node_id);

    if (drag.is_press_beside_thumb)
        return start_scrollbar_press_scroll(*node, *old_scroll_offset, destination, true);

    auto new_scroll_offset = m_async_scroll_tree.set_scroll_offset(node_id, dragged_to->scroll_offset, m_scene.current_visual_context_tree(), m_scroll_state_snapshot);
    VERIFY(new_scroll_offset.has_value());
    rebuild_wheel_hit_test_targets();

    Vector<Compositing::AsyncScrollOffset> scroll_offsets;
    scroll_offsets.append({
        .stable_node_id = node->stable_node_id,
        .compositor_scroll_offset = *new_scroll_offset,
        .unadopted_scroll_delta = *new_scroll_offset - *old_scroll_offset,
        .last_relative_scroll_delta = {},
    });
    store_pending_async_scroll_offsets(scroll_offsets);

    if (!node->is_viewport)
        return PendingFrame::repainting_changes(m_async_scrolling_viewport_rect);
    m_async_scrolling_viewport_rect.set_location(new_scroll_offset->to_type<int>());
    return PendingFrame::repainting_everything(m_async_scrolling_viewport_rect);
}

void ContextState::rebuild_wheel_hit_test_targets()
{
    VERIFY(m_scene.display_list());
    m_async_scroll_tree.rebuild_wheel_hit_test_targets(
        m_scene.display_list(),
        &m_scene.visual_context_tree_for_compositing(),
        m_scroll_state_snapshot);
}

bool ContextState::visual_animations_need_frame()
{
    return m_scene.visual_animations_need_frame(m_scroll_state_snapshot, m_presenter.viewport_size());
}

bool ContextState::advance_visual_animations(MonotonicTime now)
{
    return m_scene.advance_visual_animations(now);
}

}
