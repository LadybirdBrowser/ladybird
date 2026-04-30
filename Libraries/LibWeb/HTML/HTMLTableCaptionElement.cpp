/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableCaptionElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/HTMLTableCaptionElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableCaptionElement);

HTMLTableCaptionElement::HTMLTableCaptionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableCaptionElement::~HTMLTableCaptionElement() = default;

void HTMLTableCaptionElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableCaptionElement);
    Base::initialize(realm);
}

bool HTMLTableCaptionElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::align;
}

// https://html.spec.whatwg.org/multipage/rendering.html#tables-2
void HTMLTableCaptionElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    HTMLElement::apply_presentational_hints(properties);
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            if (value == "bottom"sv)
                properties.append({ .property_id = CSS::PropertyID::CaptionSide, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Bottom) });
        }
    });
}

}
