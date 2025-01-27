/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLOListElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/Numbers.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLOListElement);

HTMLOListElement::HTMLOListElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOListElement::~HTMLOListElement() = default;

void HTMLOListElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOListElement);
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#dom-ol-start
WebIDL::Long HTMLOListElement::start()
{
    // The start IDL attribute must reflect the content attribute of the same name, with a default value of 1.
    auto content_attribute_value = get_attribute(AttributeNames::start).value_or("1"_string);
    if (auto maybe_number = HTML::parse_integer(content_attribute_value); maybe_number.has_value())
        return *maybe_number;
    return 1;
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#concept-ol-start
size_t HTMLOListElement::starting_value() const
{
    // 1. If the ol element has a start attribute, then:
    auto start = get_attribute(AttributeNames::start);
    if (start.has_value()) {
        // 1. Let parsed be the result of parsing the value of the attribute as an integer.
        auto parsed = parse_integer(start.value());

        // 2. If parsed is not an error, then return parsed.
        if (parsed.has_value())
            return parsed.value();
    }

    // 2. If the ol element has a reversed attribute, then return the number of owned li elements.
    if (has_attribute(AttributeNames::reversed)) {
        return number_of_owned_list_items();
    }

    // 3. Return 1.
    return 1;
}

bool HTMLOListElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::type;
}

void HTMLOListElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    // https://html.spec.whatwg.org/multipage/rendering.html#lists
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::type) {
            if (value == "1"sv) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::Decimal));
            } else if (value == "a"sv) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::LowerAlpha));
            } else if (value == "A"sv) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::UpperAlpha));
            } else if (value == "i"sv) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::LowerRoman));
            } else if (value == "I"sv) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::ListStyleType, CSS::CSSKeywordValue::create(CSS::Keyword::UpperRoman));
            }
        }
    });
}

}
