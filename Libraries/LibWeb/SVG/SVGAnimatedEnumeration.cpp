/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedEnumeration);

GC::Ref<SVGAnimatedEnumeration> SVGAnimatedEnumeration::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadonlySpan<Keyword> keywords, u16 initial_value)
{
    return GC::Heap::the().allocate<SVGAnimatedEnumeration>(element, move(reflected_attribute), keywords, initial_value);
}

SVGAnimatedEnumeration::SVGAnimatedEnumeration(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadonlySpan<Keyword> keywords, u16 initial_value)
    : m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_keywords(keywords)
    , m_initial_value(initial_value)
{
}

SVGAnimatedEnumeration::~SVGAnimatedEnumeration() = default;

void SVGAnimatedEnumeration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedEnumeration__baseVal
WebIDL::ExceptionOr<void> SVGAnimatedEnumeration::set_base_val(u16 base_val)
{
    // 1. Let value be the value being assigned to baseVal.
    auto value = base_val;

    // 2. If value is 0 or is not the numeric type value for any value of the reflected attribute, then throw a
    //    TypeError.
    auto keyword = [&] -> Optional<StringView> {
        if (value == 0)
            return {};
        for (auto const& mapping : m_keywords) {
            if (mapping.value == value)
                return mapping.keyword;
        }
        return {};
    }();
    if (!keyword.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid value for baseVal"_utf16 };

    // FIXME: 3. Otherwise, if the reflecting IDL attribute is orientType and value is SVG_MARKER_ORIENT_ANGLE, then set the
    //    reflected attribute to the string "0".

    // 4. Otherwise, value is the numeric type value for a specific, single keyword value for the reflected attribute.
    //    Set the reflected attribute to that value.
    m_element->set_attribute_value(m_reflected_attribute, Utf16String::from_utf8(*keyword));
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedEnumeration__baseVal
u16 SVGAnimatedEnumeration::base_or_anim_value() const
{
    // 1. Let value be the value of the reflected attribute (using the attribute's initial value if it is not present or
    //    invalid).
    auto value = m_element->get_attribute_value(m_reflected_attribute);

    // 2. Return the numeric type value for value, according to the reflecting IDL attribute's definition.
    // NB: Keywords are matched case-sensitively, and any other value is invalid.
    for (auto const& mapping : m_keywords) {
        if (value.utf16_view() == mapping.keyword)
            return mapping.value;
    }
    return m_initial_value;
}

}
