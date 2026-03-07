/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Page/ElementResizeAction.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(ResizeHandle);

GC::Ref<ResizeHandle> ResizeHandle::create(GC::Heap& heap, PaintableBox& paintable_box)
{
    return heap.allocate<ResizeHandle>(paintable_box);
}

ResizeHandle::ResizeHandle(PaintableBox& paintable_box)
    : m_paintable_box(paintable_box)
    , m_element(as<DOM::Element>(*paintable_box.dom_node()))
{
}

void ResizeHandle::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_paintable_box);
    visitor.visit(m_element);
    if (m_resize_action)
        m_resize_action->visit_edges(visitor);
}

bool ResizeHandle::contains(CSSPixelPoint position, ChromeMetrics const& metrics) const
{
    return m_paintable_box->resizer_contains(position, metrics);
}

Optional<CSS::CursorPredefined> ResizeHandle::cursor() const
{
    auto axes = m_paintable_box->physical_resize_axes();
    if (axes.vertical) {
        if (axes.horizontal) {
            if (m_paintable_box->is_chrome_mirrored())
                return CSS::CursorPredefined::SwResize;
            return CSS::CursorPredefined::SeResize;
        }
        return CSS::CursorPredefined::NsResize;
    }
    return CSS::CursorPredefined::EwResize;
}

MouseAction ResizeHandle::handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position)
{
    if (type == UIEvents::EventNames::pointermove) {
        if (!m_resize_action)
            return MouseAction::None;
    } else if (button != UIEvents::MouseButton::Primary) {
        return MouseAction::None;
    }

    if (!m_resize_action)
        m_resize_action = make<ElementResizeAction>(m_element, visual_viewport_position);
    else
        m_resize_action->handle_pointer_move(visual_viewport_position);

    if (type == UIEvents::EventNames::pointerup) {
        m_resize_action.clear();
        return MouseAction::None;
    }

    return MouseAction::CaptureInput;
}

}
