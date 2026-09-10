/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLHtmlElement);

HTMLHtmlElement::HTMLHtmlElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHtmlElement::~HTMLHtmlElement() = default;

bool HTMLHtmlElement::should_use_body_background_properties() const
{
    // https://drafts.csswg.org/css-contain-2/#contain-property
    // Additionally, when any containments are active on either the HTML <html> or <body> elements, propagation of
    // properties from the <body> element to the initial containing block, the viewport, or the canvas background, is
    // disabled. Notably, this affects:
    // - 'background' and its longhands (see CSS Backgrounds 3 § 2.11.2 The Canvas Background and the HTML <body> Element)
    // NB: Called during rendering, reading style off the layout nodes.
    auto has_containment = [](Layout::NodeWithStyle const& layout_node) {
        return !layout_node.contain().is_empty();
    };

    auto const* layout_node = unsafe_layout_node();
    if (!layout_node || has_containment(*layout_node))
        return false;

    auto const* body_element = first_child_of_type<HTML::HTMLBodyElement>();
    if (!body_element)
        return false;
    auto const* body_layout_node = body_element->unsafe_layout_node();
    if (!body_layout_node || has_containment(*body_layout_node))
        return false;

    auto background_color = layout_node->background_color();
    auto const& background_layers = layout_node->background_layers();

    return !any_of(background_layers, [](auto const& layer) { return layer.background_image != nullptr; }) && background_color == Color::Transparent;
}

}
