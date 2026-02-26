/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLHtmlElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
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

void HTMLHtmlElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLHtmlElement);
    Base::initialize(realm);
}

bool HTMLHtmlElement::should_use_body_background_properties() const
{
    // https://drafts.csswg.org/css-contain-2/#contain-property
    // Additionally, when any containments are active on either the HTML <html> or <body> elements, propagation of
    // properties from the <body> element to the initial containing block, the viewport, or the canvas background, is
    // disabled. Notably, this affects:
    // - 'background' and its longhands (see CSS Backgrounds 3 § 2.11.2 The Canvas Background and the HTML <body> Element)
    if (!computed_properties()->contain().is_empty())
        return false;

    auto* body_element = first_child_of_type<HTML::HTMLBodyElement>();
    if (body_element && !body_element->computed_properties()->contain().is_empty())
        return false;

    // NB: Called during rendering, reading background properties.
    auto background_color = unsafe_layout_node()->computed_values().background_color();
    auto const& background_layers = unsafe_layout_node()->background_layers();

    return background_layers.is_empty() && background_color == Color::Transparent;
}

}
