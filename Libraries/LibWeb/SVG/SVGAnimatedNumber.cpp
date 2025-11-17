/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedNumberPrototype.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedNumber);

GC::Ref<SVGAnimatedNumber> SVGAnimatedNumber::create(JS::Realm& realm, GC::Ref<SVGElement> element,
    DOM::QualifiedName reflected_attribute, float initial_value, SupportsSecondValue supports_second_value,
    ValueRepresented value_represented)
{
    return realm.create<SVGAnimatedNumber>(realm, element, move(reflected_attribute), initial_value,
        supports_second_value, value_represented);
}

SVGAnimatedNumber::SVGAnimatedNumber(JS::Realm& realm, GC::Ref<SVGElement> element, DOM::QualifiedName reflected_attribute,
    float initial_value, SupportsSecondValue supports_second_value, ValueRepresented value_represented)
    : PlatformObject(realm)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_initial_value(initial_value)
    , m_supports_second_value(supports_second_value)
    , m_value_represented(value_represented)
{
}

SVGAnimatedNumber::~SVGAnimatedNumber() = default;

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedNumber__baseVal
float SVGAnimatedNumber::base_val() const
{
    // On getting baseVal or animVal, the following steps are run:
    return get_base_or_anim_value();
}

// // https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedNumber__baseVal
void SVGAnimatedNumber::set_base_val(float new_value)
{
    // 1. Let value be the value being assigned to baseVal.
    auto value = new_value;

    // 2. Let new be a list of numbers.
    Vector<float, 2> new_;

    // 3. If the reflected attribute is defined to take an number followed by an optional second number, then:
    if (m_supports_second_value == SupportsSecondValue::Yes) {
        // 1. Let current be the value of the reflected attribute (using the attribute's initial value if it is not
        //    present or invalid).
        auto current = m_element->get_attribute_value(m_reflected_attribute.local_name(), m_reflected_attribute.namespace_());
        auto current_values = MUST(current.split(' '));

        // 2. Let first be the first number in current.
        auto first = current_values.size() > 0 ? parse_value_or_initial(current_values[0]) : m_initial_value;

        // 3. Let second be the second number in current if it has been explicitly specified, and if not, the implicit
        //    value as described in the definition of the attribute.
        // NB: All known usages of <number-optional-number> specify that a missing second number defaults to the value
        //     of the first number.
        auto second = current_values.size() > 1 && !current_values[1].is_empty()
            ? parse_value_or_initial(current_values[1])
            : first;

        // 4. If this SVGAnimatedNumber object reflects the first number, then set first to value. Otherwise, set second
        //    to value.
        if (m_value_represented == ValueRepresented::First)
            first = value;
        else
            second = value;

        // 5. Append first to new.
        new_.unchecked_append(first);

        // 6. Append second to new.
        new_.unchecked_append(second);
    }

    // 4. Otherwise, the reflected attribute is defined to take a single number value. Append value to new.
    else {
        new_.unchecked_append(value);
    }

    // 5. Set the content attribute to a string consisting of each number in new serialized to an implementation
    //    specific string that, if parsed as an <number> using CSS syntax, would return the value closest to the number
    //    (given the implementation's supported Precisionreal number precision), joined and separated by a single U+0020
    //    SPACE character.
    auto new_attribute_value = MUST(String::join(' ', new_));
    m_element->set_attribute_value(m_reflected_attribute.local_name(), new_attribute_value, m_reflected_attribute.prefix(), m_reflected_attribute.namespace_());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedNumber__animVal
float SVGAnimatedNumber::anim_val() const
{
    // On getting baseVal or animVal, the following steps are run:
    return get_base_or_anim_value();
}

float SVGAnimatedNumber::parse_value_or_initial(StringView number_value) const
{
    auto value = AttributeParser::parse_number_percentage(number_value);
    if (!value.has_value())
        return m_initial_value;
    return value.release_value().value();
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedNumber__baseVal
float SVGAnimatedNumber::get_base_or_anim_value() const
{
    // 1. Let value be the value of the reflected attribute (using the attribute's initial value if it is not present or
    //    invalid).
    auto value = m_element->get_attribute_value(m_reflected_attribute.local_name(), m_reflected_attribute.namespace_());

    // 2. If the reflected attribute is defined to take an number followed by an optional second number, then:
    if (m_supports_second_value == SupportsSecondValue::Yes) {
        // 1. If this SVGAnimatedNumber object reflects the first number, then return the first value in value.
        auto values = MUST(value.split(' '));
        if (values.is_empty())
            return m_initial_value;
        if (m_value_represented == ValueRepresented::First)
            return parse_value_or_initial(values[0]);

        // 2. Otherwise, this SVGAnimatedNumber object reflects the second number. Return the second value in value if
        //    it has been explicitly specified, and if not, return the implicit value as described in the definition of
        //    the attribute.
        // NB: All known usages of <number-optional-number> specify that a missing second number defaults to the value
        //     of the first number.
        VERIFY(m_value_represented == ValueRepresented::Second);
        if (values.size() > 1 && !values[1].is_empty())
            return parse_value_or_initial(values[1]);
        return parse_value_or_initial(values[0]);
    }

    // 3. Otherwise, the reflected attribute is defined to take a single number value. Return value.
    return parse_value_or_initial(value);
}

void SVGAnimatedNumber::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedNumber);
    Base::initialize(realm);
}

void SVGAnimatedNumber::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

}
