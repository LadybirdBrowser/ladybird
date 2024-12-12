/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLDivElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/HTML/HTMLDivElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDivElement);

HTMLDivElement::HTMLDivElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDivElement::~HTMLDivElement() = default;

// https://html.spec.whatwg.org/multipage/rendering.html#flow-content-3
void HTMLDivElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name.equals_ignoring_ascii_case("align"sv)) {
            if (value.equals_ignoring_ascii_case("left"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebLeft));
            else if (value.equals_ignoring_ascii_case("right"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebRight));
            else if (value.equals_ignoring_ascii_case("center"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::LibwebCenter));
            else if (value.equals_ignoring_ascii_case("justify"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Justify));
        }
    });
}

void HTMLDivElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDivElement);
}

}
