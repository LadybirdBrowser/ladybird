/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/ElementResizeAction.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::Painting {

NonnullRefPtr<ResizeHandle> ResizeHandle::create(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
{
    return adopt_ref(*new ResizeHandle(arena, slot));
}

ResizeHandle::ResizeHandle(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
    : ChromeWidget(arena, slot)
    , m_element(as<DOM::Element>(*layout_node()->dom_node()))
{
}

bool ResizeHandle::contains(CSSPixelPoint position, ChromeMetrics const& metrics) const
{
    auto* node = layout_node();
    if (!node)
        return false;
    return resizer_contains(*node, position, metrics);
}

Optional<CSS::CursorPredefined> ResizeHandle::cursor() const
{
    auto* node = layout_node();
    if (!node)
        return {};
    auto axes = physical_resize_axes(*node);
    if (axes.vertical) {
        if (axes.horizontal) {
            if (is_chrome_mirrored(*node))
                return CSS::CursorPredefined::SwResize;
            return CSS::CursorPredefined::SeResize;
        }
        return CSS::CursorPredefined::NsResize;
    }
    return CSS::CursorPredefined::EwResize;
}

MouseAction ResizeHandle::handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position)
{
    if (type == UIEvents::EventNames::pointermove) {
        if (!m_resize_action)
            return MouseAction::None;
    } else if (button != UIEvents::MouseButton::Primary) {
        return MouseAction::None;
    }

    auto element = m_element.ptr();
    if (!element || !element->is_connected()) {
        m_resize_action.clear();
        return MouseAction::None;
    }

    if (!m_resize_action)
        m_resize_action = make<ElementResizeAction>(*element, visual_viewport_position);
    else
        m_resize_action->handle_pointer_move(visual_viewport_position);

    if (type == UIEvents::EventNames::pointerup) {
        m_resize_action.clear();
        return MouseAction::None;
    }

    return MouseAction::CaptureInput;
}

}
