/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(Scrollbar);

GC::Ref<Scrollbar> Scrollbar::create(GC::Heap& heap, PaintableBox& paintable_box, PaintableBox::ScrollDirection direction)
{
    return heap.allocate<Scrollbar>(paintable_box, direction);
}

Scrollbar::Scrollbar(PaintableBox& paintable_box, PaintableBox::ScrollDirection direction)
    : m_paintable_box(paintable_box)
    , m_direction(direction)
{
}

void Scrollbar::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_paintable_box);
}

bool Scrollbar::contains(CSSPixelPoint position, ChromeMetrics const& metrics) const
{
    if (auto rect = m_paintable_box->absolute_scrollbar_rect(m_direction, is_enlarged(), metrics); rect.has_value())
        return rect->contains(position);
    return false;
}

MouseAction Scrollbar::handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position)
{
    if (type == UIEvents::EventNames::pointermove) {
        if (!m_thumb_grab_position.has_value())
            return MouseAction::None;
    } else if (button != UIEvents::MouseButton::Primary) {
        return MouseAction::None;
    }

    auto position = m_paintable_box->transform_to_local_coordinates(visual_viewport_position);
    scroll_to_mouse_position(position);
    m_paintable_box->set_needs_repaint();

    if (type == UIEvents::EventNames::pointerup) {
        m_thumb_grab_position.clear();
        return MouseAction::None;
    }

    return MouseAction::CaptureInput;
}

MouseAction Scrollbar::mouse_move(CSSPixelPoint position)
{
    if (m_thumb_grab_position.has_value()) {
        position = m_paintable_box->transform_to_local_coordinates(position);
        scroll_to_mouse_position(position);
        return MouseAction::SwallowEvent;
    }
    return MouseAction::None;
}

MouseAction Scrollbar::mouse_up(CSSPixelPoint, unsigned)
{
    m_thumb_grab_position.clear();
    m_paintable_box->set_needs_repaint();
    return MouseAction::None;
}

void Scrollbar::mouse_enter()
{
    if (m_hovered)
        return;
    m_hovered = true;
    m_paintable_box->set_needs_repaint();
}

void Scrollbar::mouse_leave()
{
    if (!m_hovered)
        return;
    m_hovered = false;
    m_paintable_box->set_needs_repaint();
}

void Scrollbar::scroll_to_mouse_position(CSSPixelPoint position)
{
    ChromeMetrics metrics = m_paintable_box->document().page().chrome_metrics();

    auto const& scroll_state = m_paintable_box->document().paintable()->scroll_state_snapshot();
    auto scrollbar_data = m_paintable_box->compute_scrollbar_data(m_direction, metrics, &scroll_state);
    VERIFY(scrollbar_data.has_value());

    auto orientation = m_direction == PaintableBox::ScrollDirection::Horizontal ? Orientation::Horizontal : Orientation::Vertical;
    auto offset_relative_to_gutter = (position - scrollbar_data->gutter_rect.location()).primary_offset_for_orientation(orientation);
    auto gutter_size = scrollbar_data->gutter_rect.primary_size_for_orientation(orientation);
    auto thumb_size = scrollbar_data->thumb_rect.primary_size_for_orientation(orientation);

    if (gutter_size < thumb_size)
        return;

    if (!m_thumb_grab_position.has_value()) {
        m_thumb_grab_position = scrollbar_data->thumb_rect.contains(position)
            ? (position - scrollbar_data->thumb_rect.location()).primary_offset_for_orientation(orientation)
            : max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
    }

    auto constrained_offset = AK::clamp(offset_relative_to_gutter - m_thumb_grab_position.value(), 0, gutter_size - thumb_size);
    auto scroll_position = constrained_offset.to_double() / (gutter_size - thumb_size).to_double();

    auto scrollable_overflow_size = m_paintable_box->scrollable_overflow_rect()->primary_size_for_orientation(orientation);
    auto padding_size = m_paintable_box->absolute_padding_box_rect().primary_size_for_orientation(orientation);
    auto scroll_position_in_pixels = CSSPixels::nearest_value_for(scroll_position * (scrollable_overflow_size - padding_size));

    auto new_scroll_offset = m_paintable_box->scroll_offset();
    new_scroll_offset.set_primary_offset_for_orientation(orientation, scroll_position_in_pixels);
    m_paintable_box->set_scroll_offset(new_scroll_offset);
}

}
