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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLParagraphElement);
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
        if (name.equals_ignoring_ascii_case("align"sv)) {
            if (value == "left"sv)
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Left));
            else if (value == "right"sv)
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Right));
            else if (value == "center"sv)
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Center));
            else if (value == "justify"sv)
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Justify));
        }
    });
}

}
