/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Painting/LabelablePaintable.h>
#include <LibWeb/UIEvents/MouseButton.h>

namespace Web::Painting {

static bool is_inside_associated_label(HTML::FormAssociatedElement& control, CSSPixelPoint position)
{
    auto labels = control.form_associated_element_to_html_element().labels();
    if (!labels)
        return false;
    for (u32 i = 0; i < labels->length(); ++i) {
        if (auto* paintable_box = as_if<PaintableBox>(labels->item(i)->paintable())) {
            if (paintable_box->absolute_rect().contains(position))
                return true;
        }
    }
    return false;
}

LabelablePaintable::LabelablePaintable(Layout::LabelableNode const& layout_node)
    : PaintableBox(layout_node)
{
}

void LabelablePaintable::set_being_pressed(bool being_pressed)
{
    if (m_being_pressed == being_pressed)
        return;
    m_being_pressed = being_pressed;
    set_needs_display();
}

Layout::FormAssociatedLabelableNode const& LabelablePaintable::layout_box() const
{
    return static_cast<Layout::FormAssociatedLabelableNode const&>(layout_node());
}

Layout::FormAssociatedLabelableNode& LabelablePaintable::layout_box()
{
    return static_cast<Layout::FormAssociatedLabelableNode&>(layout_node());
}

LabelablePaintable::DispatchEventOfSameName LabelablePaintable::handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned)
{
    if (button != UIEvents::MouseButton::Primary || !layout_box().dom_node().enabled())
        return DispatchEventOfSameName::No;

    set_being_pressed(true);
    m_tracking_mouse = true;
    navigable()->event_handler().set_mouse_event_tracking_paintable(this);
    return DispatchEventOfSameName::Yes;
}

LabelablePaintable::DispatchEventOfSameName LabelablePaintable::handle_mouseup(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    if (!m_tracking_mouse || button != UIEvents::MouseButton::Primary || !layout_box().dom_node().enabled())
        return DispatchEventOfSameName::No;

    bool is_inside_node_or_label = absolute_rect().contains(position);
    if (!is_inside_node_or_label)
        is_inside_node_or_label = is_inside_associated_label(layout_box().dom_node(), position);

    set_being_pressed(false);
    m_tracking_mouse = false;
    navigable()->event_handler().set_mouse_event_tracking_paintable(nullptr);
    return DispatchEventOfSameName::Yes;
}

LabelablePaintable::DispatchEventOfSameName LabelablePaintable::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    if (!m_tracking_mouse || !layout_box().dom_node().enabled())
        return DispatchEventOfSameName::No;

    bool is_inside_node_or_label = absolute_rect().contains(position);
    if (!is_inside_node_or_label)
        is_inside_node_or_label = is_inside_associated_label(layout_box().dom_node(), position);

    set_being_pressed(is_inside_node_or_label);
    return DispatchEventOfSameName::Yes;
}

}
