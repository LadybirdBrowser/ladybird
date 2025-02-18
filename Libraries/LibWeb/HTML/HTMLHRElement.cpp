/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLHRElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/HTML/HTMLHRElement.h>
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLHRElement);
}

bool HTMLHRElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name, HTML::AttributeNames::color, HTML::AttributeNames::noshade, HTML::AttributeNames::width);
}

void HTMLHRElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2
        if (name == HTML::AttributeNames::color || name == HTML::AttributeNames::noshade) {
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopStyle, CSS::CSSKeywordValue::create(CSS::Keyword::Solid));
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightStyle, CSS::CSSKeywordValue::create(CSS::Keyword::Solid));
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomStyle, CSS::CSSKeywordValue::create(CSS::Keyword::Solid));
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftStyle, CSS::CSSKeywordValue::create(CSS::Keyword::Solid));
        }

        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2:attr-hr-color-3
        // When an hr element has a color attribute, its value is expected to be parsed using the rules for parsing a legacy color value, and if that does not return failure,
        // the user agent is expected to treat the attribute as a presentational hint setting the element's 'color' property to the resulting color.
        if (name == HTML::AttributeNames::color) {
            if (auto parsed_value = parse_legacy_color_value(value); parsed_value.has_value()) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Color, CSS::CSSColorValue::create_from_color(*parsed_value));
            }
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#the-hr-element-2:maps-to-the-dimension-property
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

}
