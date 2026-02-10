/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedIntegerPrototype.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedInteger.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedInteger);

GC::Ref<SVGAnimatedInteger> SVGAnimatedInteger::create(JS::Realm& realm, GC::Ref<SVGElement> element,
    DOM::QualifiedName reflected_attribute, WebIDL::Long initial_value, SupportsSecondValue supports_second_value,
    ValueRepresented value_represented)
{
    return realm.create<SVGAnimatedInteger>(realm, element, move(reflected_attribute), initial_value,
        supports_second_value, value_represented);
}

SVGAnimatedInteger::SVGAnimatedInteger(JS::Realm& realm, GC::Ref<SVGElement> element, DOM::QualifiedName reflected_attribute,
    WebIDL::Long initial_value, SupportsSecondValue supports_second_value, ValueRepresented value_represented)
    : PlatformObject(realm)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_initial_value(initial_value)
    , m_supports_second_value(supports_second_value)
    , m_value_represented(value_represented)
{
}

SVGAnimatedInteger::~SVGAnimatedInteger() = default;

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedInteger__baseVal
WebIDL::Long SVGAnimatedInteger::base_val() const
{
    // On getting baseVal or animVal, the following steps are run:
    return get_base_or_anim_value();
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedInteger__baseVal
void SVGAnimatedInteger::set_base_val(WebIDL::Long new_value)
{
    // 1. Let value be the value being assigned to baseVal.
    auto value = new_value;

    // 2. Let new be a list of integers.
    Vector<WebIDL::Long, 2> new_;

    // 3. If the reflected attribute is defined to take an integer followed by an optional second integer, then:
    if (m_supports_second_value == SupportsSecondValue::Yes) {
        // 1. Let current be the value of the reflected attribute (using the attribute's initial value if it is not
        //    present or invalid).
        auto current = m_element->get_attribute_value(m_reflected_attribute.local_name(), m_reflected_attribute.namespace_());
        auto current_values = MUST(current.split(' '));

        // 2. Let first be the first integer in current.
        auto first = current_values.size() > 0 ? parse_value_or_initial(current_values[0]) : m_initial_value;

        // 3. Let second be the second integer in current if it has been explicitly specified, and if not, the implicit
        //    value as described in the definition of the attribute.
        // NB: All known usages of <number-optional-number> specify that a missing second number defaults to the value
        //     of the first number.
        auto second = current_values.size() > 1 && !current_values[1].is_empty()
            ? parse_value_or_initial(current_values[1])
            : first;

        // 4. If this SVGAnimatedInteger object reflects the first integer, then set first to value. Otherwise, set second
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

    // 4. Otherwise, the reflected attribute is defined to take a single integer value. Append value to new.
    else {
        new_.unchecked_append(value);
    }

    // 5. Set the content attribute to a string consisting of each integer in new serialized to an implementation
    //    specific string that, if parsed as an <number> using CSS syntax, would return that integer,
    //    joined and separated by a single U+0020 SPACE character.
    auto new_attribute_value = MUST(String::join(' ', new_));
    m_element->set_attribute_value(m_reflected_attribute.local_name(), new_attribute_value, m_reflected_attribute.prefix(), m_reflected_attribute.namespace_());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedInteger__animVal
WebIDL::Long SVGAnimatedInteger::anim_val() const
{
    // On getting baseVal or animVal, the following steps are run:
    return get_base_or_anim_value();
}

WebIDL::Long SVGAnimatedInteger::parse_value_or_initial(StringView number_value) const
{
    auto value = AttributeParser::parse_integer(number_value);
    if (!value.has_value())
        return m_initial_value;
    return value.release_value();
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedInteger__baseVal
WebIDL::Long SVGAnimatedInteger::get_base_or_anim_value() const
{
    // 1. Let value be the value of the reflected attribute (using the attribute's initial value if it is not present or
    //    invalid).
    auto value = m_element->get_attribute_value(m_reflected_attribute.local_name(), m_reflected_attribute.namespace_());

    // 2. If the reflected attribute is defined to take an integer followed by an optional second integer, then:
    if (m_supports_second_value == SupportsSecondValue::Yes) {
        // 1. If this SVGAnimatedInteger object reflects the first integer, then return the first value in value.
        auto values = MUST(value.split(' '));
        if (values.is_empty())
            return m_initial_value;
        if (m_value_represented == ValueRepresented::First)
            return parse_value_or_initial(values[0]);

        // 2. Otherwise, this SVGAnimatedInteger object reflects the second integer. Return the second value in value if
        //    it has been explicitly specified, and if not, return the implicit value as described in the definition of
        //    the attribute.
        // NB: All known usages of <number-optional-number> specify that a missing second number defaults to the value
        //     of the first number.
        VERIFY(m_value_represented == ValueRepresented::Second);
        if (values.size() > 1 && !values[1].is_empty())
            return parse_value_or_initial(values[1]);
        return parse_value_or_initial(values[0]);
    }

    // 3. Otherwise, the reflected attribute is defined to take a single integer value. Return value.
    return parse_value_or_initial(value);
}

void SVGAnimatedInteger::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedInteger);
    Base::initialize(realm);
}

void SVGAnimatedInteger::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

}
