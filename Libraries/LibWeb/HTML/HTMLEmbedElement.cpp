/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLEmbedElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLEmbedElement);

HTMLEmbedElement::HTMLEmbedElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLEmbedElement::~HTMLEmbedElement() = default;

void HTMLEmbedElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLEmbedElement);
    Base::initialize(realm);
}

bool HTMLEmbedElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::height,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::width);
}

void HTMLEmbedElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images
            // When an embed, iframe, img, or object element, or an input element whose type attribute is in the Image Button state,
            // has an align attribute whose value is an ASCII case-insensitive match for the string "center" or the string "middle",
            // the user agent is expected to act as if the element's 'vertical-align' property was set to a value that aligns the
            // vertical middle of the element with the parent element's baseline.
            // FIXME: This should use legacy baseline-middle alignment instead of CSS vertical-align: middle,
            //        as Firefox and Chrome do with engine-specific legacy values.
            if (value.equals_ignoring_ascii_case("center"sv) || value.equals_ignoring_ascii_case("middle"sv))
                properties.append({ .property_id = CSS::PropertyID::VerticalAlign, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Middle) });
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                properties.append({ .property_id = CSS::PropertyID::Height, .value = *parsed_value });
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::MarginLeft, .value = *parsed_value });
                properties.append({ .property_id = CSS::PropertyID::MarginRight, .value = *parsed_value });
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::MarginTop, .value = *parsed_value });
                properties.append({ .property_id = CSS::PropertyID::MarginBottom, .value = *parsed_value });
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                properties.append({ .property_id = CSS::PropertyID::Width, .value = *parsed_value });
            }
        }
    });
}

void HTMLEmbedElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

}
