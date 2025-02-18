/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLUListElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLUListElement);
}

bool HTMLUListElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::type;
}

void HTMLUListElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#lists
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::type) {
            if (value.equals_ignoring_ascii_case("none"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::None));
            } else if (value.equals_ignoring_ascii_case("disc"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::Disc));
            } else if (value.equals_ignoring_ascii_case("circle"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::Circle));
            } else if (value.equals_ignoring_ascii_case("square"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::Square));
            }
        }
    });
}

}
