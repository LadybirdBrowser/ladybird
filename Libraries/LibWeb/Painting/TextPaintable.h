/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class TextPaintable final : public Paintable {
    GC_CELL(TextPaintable, Paintable);
    GC_DECLARE_ALLOCATOR(TextPaintable);

public:
    static GC::Ref<TextPaintable> create(Layout::TextNode const&, String const& text_for_rendering);

    Layout::TextNode const& layout_node() const { return static_cast<Layout::TextNode const&>(Paintable::layout_node()); }

    virtual bool wants_mouse_events() const override;
    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;

    String const& text_for_rendering() const { return m_text_for_rendering; }

private:
    virtual bool is_text_paintable() const override { return true; }

    TextPaintable(Layout::TextNode const&, String const& text_for_rendering);

    String m_text_for_rendering;
};

}
