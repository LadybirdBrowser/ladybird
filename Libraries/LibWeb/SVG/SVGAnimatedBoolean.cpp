/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedBoolean.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedBoolean);

GC::Ref<SVGAnimatedBoolean> SVGAnimatedBoolean::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, bool initial_value)
{
    return GC::Heap::the().allocate<SVGAnimatedBoolean>(element, move(reflected_attribute), initial_value);
}

SVGAnimatedBoolean::SVGAnimatedBoolean(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, bool initial_value)
    : m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
    , m_initial_value(initial_value)
{
}

SVGAnimatedBoolean::~SVGAnimatedBoolean() = default;

void SVGAnimatedBoolean::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedBoolean__baseVal
bool SVGAnimatedBoolean::base_or_anim_value() const
{
    // 1. Let value be the value of the reflected attribute, or the empty string if it is not present.
    auto value = m_element->get_attribute_value(m_reflected_attribute);

    // 2. If value is not "true" or "false", then set value to the reflected attribute's initial value.
    if (value != "true"sv && value != "false"sv)
        return m_initial_value;

    // 3. Return true if value is "true", and false otherwise.
    return value == "true"sv;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedBoolean__baseVal
void SVGAnimatedBoolean::set_base_val(bool value)
{
    // On setting baseVal, the reflected attribute is set to "true" if the value is true, and "false" otherwise.
    m_element->set_attribute_value(m_reflected_attribute, value ? "true"_utf16 : "false"_utf16);
}

}
