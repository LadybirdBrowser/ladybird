/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::Painting {

NonnullRefPtr<Scrollbar> Scrollbar::create(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot, ScrollDirection direction)
{
    return adopt_ref(*new Scrollbar(arena, slot, direction));
}

Scrollbar::Scrollbar(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot, ScrollDirection direction)
    : ChromeWidget(arena, slot)
    , m_direction(direction)
{
}

MouseAction Scrollbar::handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position)
{
    if (type == UIEvents::EventNames::pointermove || type == UIEvents::EventNames::pointerup) {
        if (!m_thumb_grab_position.has_value())
            return MouseAction::None;
    }
    if (type != UIEvents::EventNames::pointermove && button != UIEvents::MouseButton::Primary)
        return MouseAction::None;

    auto* node = layout_node();
    if (!node) {
        release_thumb_grab();
        return MouseAction::None;
    }

    auto position = Painting::transform_to_local_coordinates(*node, visual_viewport_position);
    if (!scroll_to_mouse_position(position) && !m_thumb_grab_position.has_value())
        return MouseAction::None;
    Painting::set_needs_repaint(*node);

    if (type == UIEvents::EventNames::pointerup) {
        release_thumb_grab();
        return MouseAction::None;
    }

    return MouseAction::CaptureInput;
}

MouseAction Scrollbar::mouse_move(CSSPixelPoint position)
{
    if (m_thumb_grab_position.has_value()) {
        auto* node = layout_node();
        if (!node)
            return MouseAction::None;
        position = Painting::transform_to_local_coordinates(*node, position);
        scroll_to_mouse_position(position);
        return MouseAction::SwallowEvent;
    }
    return MouseAction::None;
}

MouseAction Scrollbar::mouse_up(CSSPixelPoint, unsigned)
{
    release_thumb_grab();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node);
    return MouseAction::None;
}

void Scrollbar::release_thumb_grab()
{
    m_thumb_grab_position.clear();
    m_thumb_grab_gesture_hold = nullptr;
    push_enlarged_state();
}

void Scrollbar::push_enlarged_state()
{
    auto* node = layout_node();
    if (!node)
        return;
    Layout::RustFFI::layout_arena_paintable_set_scrollbar_enlarged(
        node->arena_handle(), committed_row_slot(*node), static_cast<Layout::RustFFI::ScrollDirection>(m_direction), is_enlarged());
}

void Scrollbar::mouse_enter()
{
    if (m_hovered)
        return;
    m_hovered = true;
    push_enlarged_state();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node);
}

void Scrollbar::mouse_leave()
{
    if (!m_hovered)
        return;
    m_hovered = false;
    push_enlarged_state();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node);
}

bool Scrollbar::scroll_to_mouse_position(CSSPixelPoint position)
{
    auto* node = layout_node();
    if (!node)
        return false;
    ChromeMetrics metrics = node->document().page().chrome_metrics();

    auto const& scroll_state = node->document().scroll_state_snapshot();
    auto scrollbar_data = compute_scrollbar_data(*node, m_direction, metrics, &scroll_state,
        is_enlarged() ? ScrollbarSizing::Enlarged : ScrollbarSizing::Regular);
    if (!scrollbar_data.has_value())
        return false;

    auto orientation = m_direction == ScrollDirection::Horizontal ? Orientation::Horizontal : Orientation::Vertical;
    auto offset_relative_to_gutter = (position - scrollbar_data->gutter_rect.location()).primary_offset_for_orientation(orientation);
    auto gutter_size = scrollbar_data->gutter_rect.primary_size_for_orientation(orientation);
    auto thumb_size = scrollbar_data->thumb_rect.primary_size_for_orientation(orientation);

    if (gutter_size <= thumb_size)
        return true;

    if (!m_thumb_grab_position.has_value()) {
        auto primary_position = position.primary_offset_for_orientation(orientation);
        auto position_is_along_thumb = orientation == Orientation::Vertical
            ? scrollbar_data->thumb_rect.contains_vertically(primary_position)
            : scrollbar_data->thumb_rect.contains_horizontally(primary_position);

        m_thumb_grab_position = position_is_along_thumb
            ? (position - scrollbar_data->thumb_rect.location()).primary_offset_for_orientation(orientation)
            : max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
        push_enlarged_state();
        if (auto navigable = node->document().navigable())
            m_thumb_grab_gesture_hold = make<HTML::UserScrollGestureHold>(*navigable);
    }

    auto constrained_offset = AK::clamp(offset_relative_to_gutter - m_thumb_grab_position.value(), 0, gutter_size - thumb_size);
    auto scroll_position = constrained_offset.to_double() / (gutter_size - thumb_size).to_double();

    auto scrollable_overflow_size = Painting::scrollable_overflow_rect(*node)->primary_size_for_orientation(orientation);
    auto padding_size = Painting::absolute_padding_box_rect(*node).primary_size_for_orientation(orientation);
    auto minimum_scroll_offset = Painting::minimum_scroll_offset(*node).primary_offset_for_orientation(orientation);
    auto scroll_position_in_pixels = minimum_scroll_offset + CSSPixels::nearest_value_for(scroll_position * (scrollable_overflow_size - padding_size));

    auto new_scroll_offset = Painting::scroll_offset(*node);
    new_scroll_offset.set_primary_offset_for_orientation(orientation, scroll_position_in_pixels);

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // Common examples of absolute scrolls include:
    //     manipulating the scrollbar "thumb" explicitly
    if (auto navigable = node->document().navigable())
        navigable->note_user_scroll_input_intent(Painting::SnapSelectionStrategy::Type::EndPosition);

    Painting::set_scroll_offset_from_user_input(*node, new_scroll_offset);
    return true;
}

void Scrollbar::did_detach()
{
    m_hovered = false;
    release_thumb_grab();
}

}
