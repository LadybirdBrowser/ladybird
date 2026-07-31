/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SaturatingMath.h>
#include <LibWeb/Bindings/HTMLOListElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLOListElement);

HTMLOListElement::HTMLOListElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOListElement::~HTMLOListElement() = default;

void HTMLOListElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOListElement);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#dom-ol-start
WebIDL::Long HTMLOListElement::start()
{
    // The start IDL attribute must reflect the content attribute of the same name, with a default value of 1.
    auto content_attribute_value = get_attribute(AttributeNames::start).value_or("1"_utf16);
    if (auto maybe_number = HTML::parse_integer(content_attribute_value); maybe_number.has_value())
        return *maybe_number;
    return 1;
}

void HTMLOListElement::set_start(WebIDL::Long start)
{
    set_attribute_value(AttributeNames::start, Utf16String::number(start));
}

bool HTMLOListElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name.is_one_of(HTML::AttributeNames::type, HTML::AttributeNames::start, HTML::AttributeNames::reversed);
}

void HTMLOListElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);

    // https://html.spec.whatwg.org/multipage/rendering.html#lists
    // "When an ol element has a start attribute or a reversed attribute, or both, the user agent is
    // expected to use the following steps to treat the attributes as a presentational hint for the
    // 'counter-reset' property:
    // 1. Let value be null.
    // 2. If the element has a start attribute, then set value to the result of parsing the
    //    attribute's value using the rules for parsing integers.
    // 3. If the element has a reversed attribute:
    //    1. If value is an integer, then increment value by 1 and return reversed(list-item) value.
    //    2. Otherwise, return reversed(list-item).
    // 4. Otherwise:
    //    1. If value is an integer, then decrement value by 1 and return list-item value.
    //    2. Otherwise, there is no presentational hint."
    auto parsed_start = [&]() -> Optional<i32> {
        if (auto start = get_attribute(AttributeNames::start); start.has_value())
            return parse_integer(*start);
        return {};
    }();
    if (has_attribute(AttributeNames::reversed)) {
        CSS::CounterDefinition definition { CSS::list_item_counter_name(), true, nullptr };
        if (parsed_start.has_value())
            definition.value = CSS::IntegerStyleValue::create(AK::saturating_add(*parsed_start, 1));
        properties.append({ .property_id = CSS::PropertyID::CounterReset, .value = CSS::CounterDefinitionsStyleValue::create({ move(definition) }) });
    } else if (parsed_start.has_value()) {
        CSS::CounterDefinition definition { CSS::list_item_counter_name(), false, CSS::IntegerStyleValue::create(AK::saturating_sub(*parsed_start, 1)) };
        properties.append({ .property_id = CSS::PropertyID::CounterReset, .value = CSS::CounterDefinitionsStyleValue::create({ move(definition) }) });
    }

    for_each_attribute([&](Utf16FlyString const& name, Utf16View value) {
        if (name == HTML::AttributeNames::type) {
            if (value == u"1"sv) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("decimal"_utf16_fly_string) });
            } else if (value == u"a"sv) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("lower-alpha"_utf16_fly_string) });
            } else if (value == u"A"sv) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("upper-alpha"_utf16_fly_string) });
            } else if (value == u"i"sv) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("lower-roman"_utf16_fly_string) });
            } else if (value == u"I"sv) {
                properties.append({ .property_id = CSS::PropertyID::ListStyleType, .value = CSS::CounterStyleStyleValue::create("upper-roman"_utf16_fly_string) });
            }
        }
    });
}

}
