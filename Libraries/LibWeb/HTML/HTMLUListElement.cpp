/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLUListElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/HTMLUListElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLUListElement);

HTMLUListElement::HTMLUListElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLUListElement::~HTMLUListElement() = default;

void HTMLUListElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLUListElement);
    Base::initialize(realm);
}

bool HTMLUListElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::type;
}

void HTMLUListElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);

    // https://html.spec.whatwg.org/multipage/rendering.html#lists
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::type) {
            if (value.equals_ignoring_ascii_case("none"sv)) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::KeywordStyleValue::create(CSS::Keyword::None) });
            } else if (value.equals_ignoring_ascii_case("disc"sv)) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("disc"_fly_string) });
            } else if (value.equals_ignoring_ascii_case("circle"sv)) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("circle"_fly_string) });
            } else if (value.equals_ignoring_ascii_case("square"sv)) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("square"_fly_string) });
            }
        }
    });
}

}
