/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLMarqueeElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/HTML/HTMLMarqueeElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLMarqueeElement);

HTMLMarqueeElement::HTMLMarqueeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMarqueeElement::~HTMLMarqueeElement() = default;

void HTMLMarqueeElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMarqueeElement);
}

void HTMLMarqueeElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    HTMLElement::apply_presentational_hints(style);
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::bgcolor) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-marquee-element-2:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                style.set_property(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name == HTML::AttributeNames::height) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-marquee-element-2:maps-to-the-dimension-property
            if (auto parsed_value = parse_dimension_value(value)) {
                style.set_property(CSS::PropertyID::Height, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::hspace) {
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

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-marquee-scrollamount
WebIDL::UnsignedLong HTMLMarqueeElement::scroll_amount()
{
    // The scrollAmount IDL attribute must reflect the scrollamount content attribute. The default value is 6.
    if (auto scroll_amount_string = get_attribute(HTML::AttributeNames::scrollamount); scroll_amount_string.has_value()) {
        if (auto scroll_amount = parse_non_negative_integer(*scroll_amount_string); scroll_amount.has_value())
            return *scroll_amount;
    }
    return 6;
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-marquee-scrollamount
WebIDL::ExceptionOr<void> HTMLMarqueeElement::set_scroll_amount(WebIDL::UnsignedLong value)
{
    return set_attribute(HTML::AttributeNames::scrollamount, String::number(value));
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-marquee-scrolldelay
WebIDL::UnsignedLong HTMLMarqueeElement::scroll_delay()
{
    // The scrollDelay IDL attribute must reflect the scrolldelay content attribute. The default value is 85.
    if (auto scroll_delay_string = get_attribute(HTML::AttributeNames::scrolldelay); scroll_delay_string.has_value()) {
        if (auto scroll_delay = parse_non_negative_integer(*scroll_delay_string); scroll_delay.has_value())
            return *scroll_delay;
    }
    return 85;
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-marquee-scrolldelay
WebIDL::ExceptionOr<void> HTMLMarqueeElement::set_scroll_delay(WebIDL::UnsignedLong value)
{
    return set_attribute(HTML::AttributeNames::scrolldelay, String::number(value));
}

}
