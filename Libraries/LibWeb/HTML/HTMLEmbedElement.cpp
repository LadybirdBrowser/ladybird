/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLEmbedElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLEmbedElement);

HTMLEmbedElement::HTMLEmbedElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLEmbedElement::~HTMLEmbedElement() = default;

void HTMLEmbedElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLEmbedElement);
}

void HTMLEmbedElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv))
                style.set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Center));
            else if (value.equals_ignoring_ascii_case("middle"sv))
                style.set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Middle));
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                style.set_property(CSS::PropertyID::Height, *parsed_value);
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                style.set_property(CSS::PropertyID::MarginLeft, *parsed_value);
                style.set_property(CSS::PropertyID::MarginRight, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                style.set_property(CSS::PropertyID::MarginTop, *parsed_value);
                style.set_property(CSS::PropertyID::MarginBottom, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                style.set_property(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

}
