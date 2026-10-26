/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/ElasticOverscrollController.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>

namespace Compositor {

void ElasticOverscrollController::set_enabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled)
        forget();
}

bool ElasticOverscrollController::step_can_stretch(Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase) const
{
    if (!m_enabled)
        return false;
    // Only a touchpad gesture says when the fingers lift, which is when a stretch has to spring back.
    if (wheel_delta_precision != Web::WheelDeltaPrecision::Precise)
        return false;
    return scroll_gesture_phase == Web::ScrollGesturePhase::Ongoing || scroll_gesture_phase == Web::ScrollGesturePhase::Momentum;
}

void ElasticOverscrollController::note_step_from_ui(Gfx::FloatPoint delta, MonotonicTime now)
{
    m_step_velocity_tracker.add_step(delta, now);
}

ElasticOverscrollController::ActiveOverscroll* ElasticOverscrollController::active_overscroll_of(Web::AsyncScrollNodeStableID stable_node_id)
{
    for (auto& active : m_active_overscrolls) {
        if (active.stable_node_id == stable_node_id)
            return &active;
    }
    return nullptr;
}

bool ElasticOverscrollController::has_active_overscroll_of(Web::AsyncScrollNodeStableID stable_node_id) const
{
    return any_of(m_active_overscrolls, [&](auto const& active) { return active.stable_node_id == stable_node_id; });
}

Gfx::FloatPoint ElasticOverscrollController::stretch_of(Web::AsyncScrollNodeStableID stable_node_id) const
{
    for (auto const& active : m_active_overscrolls) {
        if (active.stable_node_id == stable_node_id)
            return active.overscroll.stretch();
    }
    return {};
}

Gfx::FloatPoint ElasticOverscrollController::relax_by(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint step_delta)
{
    auto* active = active_overscroll_of(stable_node_id);
    if (!active)
        return step_delta;
    auto delta_left_to_scroll_with = active->overscroll.relax_by(step_delta);
    if (active->overscroll.is_at_rest())
        m_active_overscrolls.remove_all_matching([&](auto const& candidate) { return candidate.stable_node_id == stable_node_id; });
    return delta_left_to_scroll_with;
}

bool ElasticOverscrollController::pull_by(Compositing::AsyncScrollNode const& node, Gfx::FloatPoint scroll_offset, Gfx::FloatPoint step_delta, Gfx::FloatPoint delta_the_scroller_did_not_scroll_by, float visual_viewport_magnification)
{
    // A touchpad step is rarely along one axis exactly. Only its main direction pulls, so that a vertical pan next to
    // a side edge does not creep past that edge.
    Gfx::FloatPoint overscroll_delta;
    bool step_is_mainly_vertical = AK::abs(step_delta.y()) >= AK::abs(step_delta.x());
    if (step_is_mainly_vertical) {
        auto delta_y = delta_the_scroller_did_not_scroll_by.y();
        bool is_at_the_edge_the_delta_points_past = (delta_y < 0 && scroll_offset.y() <= node.min_scroll_offset.y())
            || (delta_y > 0 && scroll_offset.y() >= node.max_scroll_offset.y());
        if (node.can_be_wheel_scrolled_vertically && node.shows_overscroll_effect_vertically && is_at_the_edge_the_delta_points_past)
            overscroll_delta.set_y(delta_y);
    } else {
        auto delta_x = delta_the_scroller_did_not_scroll_by.x();
        bool is_at_the_edge_the_delta_points_past = (delta_x < 0 && scroll_offset.x() <= node.min_scroll_offset.x())
            || (delta_x > 0 && scroll_offset.x() >= node.max_scroll_offset.x());
        if (node.can_be_wheel_scrolled_horizontally && node.shows_overscroll_effect_horizontally && is_at_the_edge_the_delta_points_past)
            overscroll_delta.set_x(delta_x);
    }
    if (overscroll_delta.is_zero())
        return false;

    auto* active = active_overscroll_of(node.stable_node_id);
    if (!active) {
        // A zoomed page shows only the part of the viewport's scrollport that its visual viewport is on, and that part
        // is what bounds how far the viewport stretches.
        auto scrollport_size = node.scrollport_rect.size().to_type<float>();
        if (node.is_viewport)
            scrollport_size.scale_by(1.0f / visual_viewport_magnification);
        m_active_overscrolls.append({ node.stable_node_id, Compositing::ElasticOverscroll { scrollport_size, visual_viewport_magnification } });
        active = &m_active_overscrolls.last();
    }
    active->overscroll.pull_by(overscroll_delta);
    return true;
}

void ElasticOverscrollController::release_held_by_fingers(MonotonicTime now)
{
    auto velocity = m_step_velocity_tracker.velocity_at_lift(now).value_or({});
    for (auto& active : m_active_overscrolls)
        active.overscroll.release(velocity, now);
}

void ElasticOverscrollController::drop_stretch_moved_by(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint scroll_delta)
{
    auto stretch = stretch_of(stable_node_id);
    if ((stretch.x() != 0 && scroll_delta.x() != 0) || (stretch.y() != 0 && scroll_delta.y() != 0))
        m_active_overscrolls.remove_all_matching([&](auto const& active) { return active.stable_node_id == stable_node_id; });
}

void ElasticOverscrollController::release_stretch_of(Web::AsyncScrollNodeStableID stable_node_id, MonotonicTime now)
{
    if (auto* active = active_overscroll_of(stable_node_id); active && !active->overscroll.is_rebounding())
        active->overscroll.release({}, now);
}

void ElasticOverscrollController::end_gesture(MonotonicTime now)
{
    release_held_by_fingers(now);
    m_step_velocity_tracker.reset();
}

void ElasticOverscrollController::forget()
{
    m_active_overscrolls.clear();
    m_step_velocity_tracker.reset();
}

void ElasticOverscrollController::did_install_scrolling_state(Compositing::AsyncScrollTree const& async_scroll_tree)
{
    m_active_overscrolls.remove_all_matching([&](auto const& active) {
        auto node_id = async_scroll_tree.scroll_node_id_for_stable_id(active.stable_node_id);
        auto const* node = node_id.has_value() ? async_scroll_tree.scroll_node_for_id(*node_id) : nullptr;
        if (!node)
            return true;
        // A scroller that has been told not to stretch along an axis since it was pulled lets go at once.
        auto stretch = active.overscroll.stretch();
        return (stretch.x() != 0 && !node->shows_overscroll_effect_horizontally) || (stretch.y() != 0 && !node->shows_overscroll_effect_vertically);
    });
}

bool ElasticOverscrollController::has_rebound() const
{
    return any_of(m_active_overscrolls, [](auto const& active) { return active.overscroll.is_rebounding(); });
}

bool ElasticOverscrollController::advance_rebounds(MonotonicTime frame_time)
{
    bool a_presented_stretch_changed = false;
    m_active_overscrolls.remove_all_matching([&](auto& active) {
        if (!active.overscroll.is_rebounding())
            return false;
        auto stretch_before = active.overscroll.stretch();
        bool is_still_rebounding = active.overscroll.advance_rebound_to(frame_time);
        if (active.overscroll.stretch() != stretch_before)
            a_presented_stretch_changed = true;
        return !is_still_rebounding;
    });
    return a_presented_stretch_changed;
}

Optional<Compositing::ScrollStateSnapshot> ElasticOverscrollController::stretched_scroll_state_snapshot(Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Compositing::AsyncScrollTree const& async_scroll_tree) const
{
    Optional<Compositing::ScrollStateSnapshot> stretched_scroll_state_snapshot;
    for (auto const& active : m_active_overscrolls) {
        auto stretch = active.overscroll.stretch();
        if (stretch.is_zero())
            continue;
        auto node_id = async_scroll_tree.scroll_node_id_for_stable_id(active.stable_node_id);
        if (!node_id.has_value())
            continue;
        if (!stretched_scroll_state_snapshot.has_value())
            stretched_scroll_state_snapshot = scroll_state_snapshot;
        // Only the scroller's own entry moves. The sticky boxes in it keep the shifts resolved for the scroll offset
        // it really has, so they move with its content.
        auto index = node_id->scroll_node_index;
        stretched_scroll_state_snapshot->set_device_offset_for_index(index, stretched_scroll_state_snapshot->device_offset_for_index(index) - stretch);
    }
    return stretched_scroll_state_snapshot;
}

}
