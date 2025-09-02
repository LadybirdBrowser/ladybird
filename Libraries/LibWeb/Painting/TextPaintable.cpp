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

GC::Ref<TextPaintable> TextPaintable::create(Layout::TextNode const& layout_node)
{
    return layout_node.heap().allocate<TextPaintable>(layout_node);
}

TextPaintable::TextPaintable(Layout::TextNode const& layout_node)
    : Paintable(layout_node)
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

void TextPaintable::paint_inspector_overlay_internal(DisplayListRecordingContext& context) const
{
    if (auto const* parent_paintable = as_if<PaintableWithLines>(parent())) {
        for (auto const& fragment : parent_paintable->fragments()) {
            if (&fragment.paintable() == this) {
                paint_text_fragment_debug_highlight(context, fragment);
            }
        }
    }
}

}
