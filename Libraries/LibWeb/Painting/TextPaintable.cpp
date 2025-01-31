/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Layout/LabelableNode.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(TextPaintable);

GC::Ref<TextPaintable> TextPaintable::create(Layout::TextNode const& layout_node, String const& text_for_rendering)
{
    return layout_node.heap().allocate<TextPaintable>(layout_node, text_for_rendering);
}

TextPaintable::TextPaintable(Layout::TextNode const& layout_node, String const& text_for_rendering)
    : Paintable(layout_node)
    , m_text_for_rendering(text_for_rendering)
{
}

bool TextPaintable::wants_mouse_events() const
{
    return layout_node().first_ancestor_of_type<Layout::Label>();
}

TextPaintable::DispatchEventOfSameName TextPaintable::handle_mousedown(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    auto* label = layout_node().first_ancestor_of_type<Layout::Label>();
    if (!label)
        return DispatchEventOfSameName::No;
    const_cast<Layout::Label*>(label)->handle_mousedown_on_label({}, position, button);
    navigable()->event_handler().set_mouse_event_tracking_paintable(this);
    return DispatchEventOfSameName::Yes;
}

TextPaintable::DispatchEventOfSameName TextPaintable::handle_mouseup(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    auto* label = layout_node().first_ancestor_of_type<Layout::Label>();
    if (!label)
        return DispatchEventOfSameName::No;

    const_cast<Layout::Label*>(label)->handle_mouseup_on_label({}, position, button);
    navigable()->event_handler().set_mouse_event_tracking_paintable(nullptr);
    return DispatchEventOfSameName::Yes;
}

TextPaintable::DispatchEventOfSameName TextPaintable::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    auto* label = layout_node().first_ancestor_of_type<Layout::Label>();
    if (!label)
        return DispatchEventOfSameName::No;
    const_cast<Layout::Label*>(label)->handle_mousemove_on_label({}, position, button);
    return DispatchEventOfSameName::Yes;
}

}
