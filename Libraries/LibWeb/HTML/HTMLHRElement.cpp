/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLHRElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLHRElement);

HTMLHRElement::HTMLHRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHRElement::~HTMLHRElement() = default;

void HTMLHRElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLHRElement);
    Base::initialize(realm);
}

bool HTMLHRElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name, HTML::AttributeNames::align, HTML::AttributeNames::color, HTML::AttributeNames::noshade, HTML::AttributeNames::width, HTML::AttributeNames::size);
}

void HTMLHRElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2
        if (name == HTML::AttributeNames::color || name == HTML::AttributeNames::noshade) {
            properties.append({ .property_id = CSS::PropertyID::BorderTopStyle, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Solid) });
            properties.append({ .property_id = CSS::PropertyID::BorderRightStyle, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Solid) });
            properties.append({ .property_id = CSS::PropertyID::BorderBottomStyle, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Solid) });
            properties.append({ .property_id = CSS::PropertyID::BorderLeftStyle, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Solid) });
        }

        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("left"sv)) {
                properties.append({ .property_id = CSS::PropertyID::MarginLeft, .value = CSS::LengthStyleValue::create(CSS::Length::make_px(0)) });
                properties.append({ .property_id = CSS::PropertyID::MarginRight, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Auto) });
            } else if (value.equals_ignoring_ascii_case("right"sv)) {
                properties.append({ .property_id = CSS::PropertyID::MarginLeft, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Auto) });
                properties.append({ .property_id = CSS::PropertyID::MarginRight, .value = CSS::LengthStyleValue::create(CSS::Length::make_px(0)) });
            } else if (value.equals_ignoring_ascii_case("center"sv)) {
                properties.append({ .property_id = CSS::PropertyID::MarginLeft, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Auto) });
                properties.append({ .property_id = CSS::PropertyID::MarginRight, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Auto) });
            }
        }

        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2:attr-hr-color-3
        // When an hr element has a color attribute, its value is expected to be parsed using the rules for parsing a legacy color value, and if that does not return failure,
        // the user agent is expected to treat the attribute as a presentational hint setting the element's 'color' property to the resulting color.
        if (name == HTML::AttributeNames::color) {
            if (auto parsed_value = parse_legacy_color_value(value); parsed_value.has_value()) {
                properties.append({ .property_id = CSS::PropertyID::Color, .value = CSS::ColorStyleValue::create_from_color(*parsed_value, CSS::ColorSyntax::Legacy) });
            }
        }

        // If an hr element has either a color attribute or a noshade attribute, and furthermore also has a size attribute,
        // and parsing that attribute's value using the rules for parsing non-negative integers doesn't generate an error,
        // then the user agent is expected to use the parsed value divided by two as a pixel length for presentational hints
        // for the properties 'border-top-width', 'border-right-width', 'border-bottom-width', and 'border-left-width' on the element.
        bool has_color_or_noshade = has_attribute(HTML::AttributeNames::color) || has_attribute(HTML::AttributeNames::noshade);
        if (name == HTML::AttributeNames::size && has_color_or_noshade) {
            if (auto parsed_value = parse_non_negative_integer(value); parsed_value.has_value()) {
                auto size_value = CSS::LengthStyleValue::create(CSS::Length::make_px(parsed_value.value() / 2.0));
                properties.append({ .property_id = CSS::PropertyID::BorderTopWidth, .value = size_value });
                properties.append({ .property_id = CSS::PropertyID::BorderRightWidth, .value = size_value });
                properties.append({ .property_id = CSS::PropertyID::BorderBottomWidth, .value = size_value });
                properties.append({ .property_id = CSS::PropertyID::BorderLeftWidth, .value = size_value });
            }

        } else if (name == HTML::AttributeNames::size && !has_color_or_noshade) {
            // Otherwise, if an hr element has neither a color attribute nor a noshade attribute, but does have a size attribute,
            // and parsing that attribute's value using the rules for parsing non-negative integers doesn't generate an error,
            // then: if the parsed value is one, then the user agent is expected to use the attribute as a presentational hint
            // setting the element's 'border-bottom-width' to 0; otherwise, if the parsed value is greater than one,
            // then the user agent is expected to use the parsed value minus two as a pixel length for presentational hints
            // for the 'height' property on the element.
            if (auto parsed_value = parse_non_negative_integer(value); parsed_value.has_value()) {
                if (parsed_value.value() == 1) {
                    properties.append({ .property_id = CSS::PropertyID::BorderBottomWidth, .value = CSS::LengthStyleValue::create(CSS::Length::make_px(0)) });
                } else if (parsed_value.value() > 1) {
                    properties.append({ .property_id = CSS::PropertyID::Height, .value = CSS::LengthStyleValue::create(CSS::Length::make_px(parsed_value.value() - 2)) });
                }
            }
        }

        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2:maps-to-the-dimension-property
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value))
                properties.append({ .property_id = CSS::PropertyID::Width, .value = *parsed_value });
        }
    });
}

}
