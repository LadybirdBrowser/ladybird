/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Layout/LabelableNode.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/LabelablePaintable.h>
#include <LibWeb/UIEvents/MouseButton.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(Label);

Label::Label(DOM::Document& document, HTML::HTMLLabelElement* element, NonnullRefPtr<CSS::StyleProperties> style)
    : BlockContainer(document, element, move(style))
{
}

Label::~Label() = default;

void Label::handle_mousedown_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint, unsigned button)
{
    if (button != UIEvents::MouseButton::Primary)
        return;

    if (auto control = dom_node().control(); control && is<Painting::LabelablePaintable>(control->paintable())) {
        auto& labelable_paintable = verify_cast<Painting::LabelablePaintable>(*control->paintable());
        labelable_paintable.handle_associated_label_mousedown({});
    }

    m_tracking_mouse = true;
}

void Label::handle_mouseup_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint position, unsigned button)
{
    if (!m_tracking_mouse || button != UIEvents::MouseButton::Primary)
        return;

    if (auto control = dom_node().control(); control && is<Painting::LabelablePaintable>(control->paintable())) {
        bool is_inside_control = control->paintable_box()->absolute_rect().contains(position);
        bool is_inside_label = paintable_box()->absolute_rect().contains(position);
        if (is_inside_control || is_inside_label) {
            auto& labelable_paintable = verify_cast<Painting::LabelablePaintable>(*control->paintable());
            labelable_paintable.handle_associated_label_mouseup({});
        }
    }

    m_tracking_mouse = false;
}

void Label::handle_mousemove_on_label(Badge<Painting::TextPaintable>, CSSPixelPoint position, unsigned)
{
    if (!m_tracking_mouse)
        return;

    if (auto control = dom_node().control(); control && is<Painting::LabelablePaintable>(control->paintable())) {
        bool is_inside_control = control->paintable_box()->absolute_rect().contains(position);
        bool is_inside_label = paintable_box()->absolute_rect().contains(position);
        auto& labelable_paintable = verify_cast<Painting::LabelablePaintable>(*control->paintable());
        labelable_paintable.handle_associated_label_mousemove({}, is_inside_control || is_inside_label);
    }
}

bool Label::is_inside_associated_label(LabelableNode const& control, CSSPixelPoint position)
{
    if (auto* label = label_for_control_node(control); label)
        return label->paintable_box()->absolute_rect().contains(position);
    return false;
}

bool Label::is_associated_label_hovered(LabelableNode const& control)
{
    if (auto* label = label_for_control_node(control); label) {
        if (label->document().hovered_node() == &label->dom_node())
            return true;

        if (auto* child = label->first_child_of_type<TextNode>(); child)
            return label->document().hovered_node() == &child->dom_node();
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/forms.html#labeled-control
Label const* Label::label_for_control_node(LabelableNode const& control)
{
    if (!control.document().layout_node())
        return nullptr;

    // The for attribute may be specified to indicate a form control with which the caption is to be associated.
    // If the attribute is specified, the attribute's value must be the ID of a labelable element in the
    // same tree as the label element. If the attribute is specified and there is an element in the tree
    // whose ID is equal to the value of the for attribute, and the first such element in tree order is
    // a labelable element, then that element is the label element's labeled control.
    if (auto const& id = control.dom_node().id(); id.has_value() && !id->is_empty()) {
        Label const* label = nullptr;

        control.document().layout_node()->for_each_in_inclusive_subtree_of_type<Label>([&](auto& node) {
            if (node.dom_node().for_() == id) {
                label = &node;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });

        if (label)
            return label;
    }

    // If the for attribute is not specified, but the label element has a labelable element descendant,
    // then the first such descendant in tree order is the label element's labeled control.
    return control.first_ancestor_of_type<Label>();
}

}
