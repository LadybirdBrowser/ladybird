/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class WEB_API Label final : public BlockContainer {
    GC_CELL(Label, BlockContainer);
    GC_DECLARE_ALLOCATOR(Label);

public:
    Label(DOM::Document&, HTML::HTMLLabelElement*, GC::Ref<CSS::ComputedProperties>);
    virtual ~Label() override;

    static bool is_inside_associated_label(LabelableNode const&, CSSPixelPoint);
    static bool is_associated_label_hovered(LabelableNode const&);

    HTML::HTMLLabelElement const& dom_node() const { return static_cast<HTML::HTMLLabelElement const&>(*BlockContainer::dom_node()); }
    HTML::HTMLLabelElement& dom_node() { return static_cast<HTML::HTMLLabelElement&>(*BlockContainer::dom_node()); }

    void handle_mousedown_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint, unsigned button);
    void handle_mouseup_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint, unsigned button);
    void handle_mousemove_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint, unsigned button);

private:
    virtual bool is_label() const override { return true; }

    static Label const* label_for_control_node(LabelableNode const&);

    bool m_tracking_mouse { false };
};

template<>
inline bool Node::fast_is<Label>() const { return is_label(); }

}
