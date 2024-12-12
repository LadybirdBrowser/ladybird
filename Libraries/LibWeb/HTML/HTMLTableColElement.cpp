/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableColElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableColElement);

HTMLTableColElement::HTMLTableColElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableColElement::~HTMLTableColElement() = default;

void HTMLTableColElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableColElement);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-colgroup-span
WebIDL::UnsignedLong HTMLTableColElement::span() const
{
    // The span IDL attribute must reflect the content attribute of the same name. It is clamped to the range [1, 1000], and its default value is 1.
    if (auto span_string = get_attribute(HTML::AttributeNames::span); span_string.has_value()) {
        if (auto span_digits = parse_non_negative_integer_digits(*span_string); span_digits.has_value()) {
            auto span = AK::StringUtils::convert_to_int<i64>(*span_digits);
            // NOTE: If span has no value at this point, the value must be larger than NumericLimits<i64>::max(), so return the maximum value of 1000.
            if (!span.has_value())
                return 1000;
            return clamp(*span, 1, 1000);
        }
    }
    return 1;
}

WebIDL::ExceptionOr<void> HTMLTableColElement::set_span(unsigned int value)
{
    if (value > 2147483647)
        value = 1;
    return set_attribute(HTML::AttributeNames::span, String::number(value));
}

void HTMLTableColElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#tables-2:maps-to-the-dimension-property-2
        if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

}
