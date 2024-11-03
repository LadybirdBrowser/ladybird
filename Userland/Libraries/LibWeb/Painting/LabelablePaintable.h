/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/FormAssociatedLabelableNode.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

// FIXME: Splinter this into FormAssociatedLabelablePaintable once
//        ProgressPaintable switches over to this.
class LabelablePaintable : public PaintableBox {
    JS_CELL(LabelablePaintable, PaintableBox);

public:
    Layout::FormAssociatedLabelableNode const& layout_box() const;
    Layout::FormAssociatedLabelableNode& layout_box();

    virtual bool wants_mouse_events() const override { return true; }
    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers) override;

    void handle_associated_label_mousedown(Badge<Layout::Label>);
    void handle_associated_label_mouseup(Badge<Layout::Label>);
    void handle_associated_label_mousemove(Badge<Layout::Label>, bool is_inside_node_or_label);

    bool being_pressed() const { return m_being_pressed; }
    // NOTE: Only the HTML node associated with this paintable should call this!
    void set_being_pressed(bool being_pressed);

protected:
    LabelablePaintable(Layout::LabelableNode const&);

private:
    bool m_being_pressed { false };
    bool m_tracking_mouse { false };
};

}
