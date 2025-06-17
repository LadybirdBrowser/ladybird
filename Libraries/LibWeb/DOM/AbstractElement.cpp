/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>

namespace Web::DOM {

AbstractElement::AbstractElement(GC::Ref<Element> element, Optional<CSS::PseudoElement> pseudo_element)
    : m_element(element)
    , m_pseudo_element(move(pseudo_element))
{
}

void AbstractElement::visit(GC::Cell::Visitor& visitor) const
{
    visitor.visit(m_element);
}

GC::Ptr<Element const> AbstractElement::parent_element() const
{
    if (m_pseudo_element.has_value())
        return m_element;
    return m_element->parent_element();
}

GC::Ptr<CSS::ComputedProperties const> AbstractElement::computed_properties() const
{
    if (m_pseudo_element.has_value())
        return m_element->pseudo_element_computed_properties(*m_pseudo_element);
    return m_element->computed_properties();
}

CSS::CountersSet& AbstractElement::ensure_counters_set()
{
    // FIXME: Handle pseudo-elements
    return m_element->ensure_counters_set();
}

void AbstractElement::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    // FIXME: Handle pseudo-elements
    m_element->set_counters_set(move(counters_set));
}

}
