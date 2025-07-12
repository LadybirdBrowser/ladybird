/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLParagraphElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLParagraphElement);

HTMLParagraphElement::HTMLParagraphElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLParagraphElement::~HTMLParagraphElement() = default;

void HTMLParagraphElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLParagraphElement);
    Base::initialize(realm);
}

bool HTMLParagraphElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::align;
}

// https://html.spec.whatwg.org/multipage/rendering.html#tables-2
void HTMLParagraphElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    HTMLElement::apply_presentational_hints(cascaded_properties);
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("left"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Left));
            else if (value.equals_ignoring_ascii_case("right"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Right));
            else if (value.equals_ignoring_ascii_case("center"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Center));
            else if (value.equals_ignoring_ascii_case("justify"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Justify));
        }
    });
}

}
