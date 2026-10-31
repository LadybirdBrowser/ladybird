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
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

namespace Web::Painting {

NonnullRefPtr<Scrollbar> Scrollbar::create(Layout::NodeArena& arena, Compositing::RustFFI::NodeSlotId slot, ScrollDirection direction)
{
    return adopt_ref(*new Scrollbar(arena, slot, direction));
}

Scrollbar::Scrollbar(Layout::NodeArena& arena, Compositing::RustFFI::NodeSlotId slot, ScrollDirection direction)
    : ChromeWidget(arena, slot)
    , m_direction(direction)
{
}

void Scrollbar::begin_drag_driven_by_compositor()
{
    m_drag_is_driven_by_compositor = true;
    push_enlarged_state();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node, InvalidateDisplayList::PaintCommands);
}

MouseAction Scrollbar::handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint)
{
    if (!m_drag_is_driven_by_compositor)
        return MouseAction::None;
    if (type != UIEvents::EventNames::pointerup || button != UIEvents::MouseButton::Primary)
        return MouseAction::CaptureInput;
    release_drag();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node, InvalidateDisplayList::PaintCommands);
    return MouseAction::None;
}

void Scrollbar::release_drag()
{
    m_drag_is_driven_by_compositor = false;
    push_enlarged_state();
}

void Scrollbar::push_enlarged_state()
{
    auto* node = layout_node();
    if (!node)
        return;
    Layout::RustFFI::render_state_set_scrollbar_enlarged(
        node->document_host(), committed_row_slot(*node), static_cast<Layout::RustFFI::ScrollDirection>(m_direction), is_enlarged());
}

void Scrollbar::mouse_enter()
{
    if (m_hovered)
        return;
    m_hovered = true;
    push_enlarged_state();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node, InvalidateDisplayList::PaintCommands);
}

void Scrollbar::mouse_leave()
{
    if (!m_hovered)
        return;
    m_hovered = false;
    push_enlarged_state();
    if (auto* node = layout_node())
        Painting::set_needs_repaint(*node, InvalidateDisplayList::PaintCommands);
}

void Scrollbar::did_detach()
{
    m_hovered = false;
    release_drag();
}

}
