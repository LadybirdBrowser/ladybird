/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLHRElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLHRElement);

HTMLHRElement::HTMLHRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHRElement::~HTMLHRElement() = default;

void HTMLHRElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLHRElement);
}

void HTMLHRElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2:maps-to-the-dimension-property
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                style.set_property(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

}
