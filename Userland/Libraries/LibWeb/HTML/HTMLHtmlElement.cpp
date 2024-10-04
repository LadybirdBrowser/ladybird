/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLHtmlElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLHtmlElement);

HTMLHtmlElement::HTMLHtmlElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHtmlElement::~HTMLHtmlElement() = default;

void HTMLHtmlElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLHtmlElement);
}

bool HTMLHtmlElement::should_use_body_background_properties() const
{
    auto background_color = layout_node()->computed_values().background_color();
    auto const& background_layers = layout_node()->background_layers();

    for (auto& layer : background_layers) {
        if (layer.background_image)
            return false;
    }

    return (background_color == Color::Transparent);
}

}
